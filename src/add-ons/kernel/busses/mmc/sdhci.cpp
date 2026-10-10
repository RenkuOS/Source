/*
 * Copyright 2018-2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 *		Ron Ben Aroya, sed4906birdie@gmail.com
 */


#include <algorithm>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver_settings.h>
#include <vm/vm.h>

#include <bus/PCI.h>
#include <ACPI.h>
#include "acpi.h"

#include <KernelExport.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"
#include "sdhci.h"


// Verbose tracing, including from the interrupt handler. Printing there slows
// interrupt handling down enough to break transfers, keep it off normally.
//#define TRACE_SDHCI
#ifdef TRACE_SDHCI
#	define TRACE(x...) dprintf("\33[33msdhci:\33[0m " x)
#else
#	define TRACE(x...) do {} while (false)
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[33msdhci:\33[0m " x)
#define ERROR(x...)			dprintf("\33[33msdhci:\33[0m " x)
#define CALLED(x...)		TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#define SDHCI_DEVICE_MODULE_NAME "busses/mmc/sdhci/driver_v1"


device_manager_info* gDeviceManager;
device_module_info* gMMCBusController;


static int32
sdhci_generic_interrupt(void* data)
{
	SdhciBus* bus = (SdhciBus*)data;
	return bus->HandleInterrupt();
}


SdhciBus::SdhciBus(struct registers* registers, uint8_t irq, bool poll)
	:
	fRegisters(registers),
	fIrq(irq),
	fWorkerThread(0),
	fCardType(CARD_TYPE_UNKNOWN),
	fDmaArea(-1),
	fBounceBuffer(NULL),
	fBouncePhysical(0),
	fTransferModeSelected(false),
	fDataMode(kDataModePio),
	fStopMode(kStopModeSingle),
	fUseDataTransferMode(false),
	fDataTransferMode(0),
	fSelectedRca(0)
{
	if (irq == 0 || irq == 0xff) {
		ERROR("IRQ not assigned\n");
		fStatus = B_BAD_DATA;
		return;
	}

	fInterruptNotifier.Init(this, "SDHCI interrupts");

	DisableInterrupts();

	fStatus = install_io_interrupt_handler(fIrq,
		sdhci_generic_interrupt, this, 0);

	if (fStatus != B_OK) {
		ERROR("can't install interrupt handler\n");
		return;
	}

	// A bounce buffer for ReadData(), which reads into kernel buffers of any
	// kind. Below 4GB, since the SDMA address register is 32 bits wide.
	fDmaArea = create_area("sdhci dma", (void**)&fBounceBuffer,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fDmaArea < 0) {
		ERROR("Could not allocate DMA memory\n");
		fStatus = fDmaArea;
		return;
	}

	physical_entry entry;
	fStatus = get_memory_map(fBounceBuffer, B_PAGE_SIZE, &entry, 1);
	if (fStatus != B_OK) {
		ERROR("Could not get physical address of DMA memory\n");
		return;
	}
	fBouncePhysical = entry.address;

	// First of all, we have to make sure we are in a sane state. The easiest
	// way is to reset everything.
	Reset();

	TRACE("Controller spec version: %d, vendor version: %#02x\n",
		fRegisters->host_controller_version.specVersion,
		fRegisters->host_controller_version.vendorVersion);

	TRACE("Capabilities: %s%s%s%s%s%s%s%s%s%s%s%s%s%s\n"
		"    Clock multiplier: %" PRIx8 "\n"
		"    Retuning modes: %" PRIx8 "\n"
		"    Retuning timer count: %" PRIx8 "\n"
		"    Slot type: %" PRIx8 "\n"
		"    Supported voltages: %" PRIx8 "\n"
		"    Max block length: %" PRIx8 "\n"
		"    Base clock frequency: %" PRId8 " MHz\n"
		"    Timeout clock: %" PRId8 " kHz\n",
		fRegisters->capabilities.UseTuningForSDR50() ? "SDR50 needs retuning, " : "",
		fRegisters->capabilities.TypeDSupport() ? "Type-D, " : "",
		fRegisters->capabilities.TypeCSupport() ? "Type-C, " : "",
		fRegisters->capabilities.TypeASupport() ? "Type-A, " : "",
		fRegisters->capabilities.DDR50Support() ? "DDR50, " : "",
		fRegisters->capabilities.SDR104Support() ? "SDR104, " : "",
		fRegisters->capabilities.SDR50Support() ? "SDR50, " : "",
		fRegisters->capabilities.AsynchronousInterrupts() ? "Asynchronous interrupts, " : "",
		fRegisters->capabilities.SystemBus64Bits() ? "64-bit system bus, " : "",
		fRegisters->capabilities.SuspendResume() ? "Suspend/Resume, " : "",
		fRegisters->capabilities.SimpleDMA() ? "Simple DMA, " : "",
		fRegisters->capabilities.HighSpeed() ? "High speed, " : "",
		fRegisters->capabilities.AdvancedDMA() ? "Advanced DMA, " : "",
		fRegisters->capabilities.Embedded8Bit() ? "8-bit Embedded mode, " : "",
		fRegisters->capabilities.ClockMultiplier(),
		fRegisters->capabilities.RetuningModes(),
		fRegisters->capabilities.RetuningTimerCount(),
		fRegisters->capabilities.SlotType(),
		fRegisters->capabilities.SupportedVoltages(),
		fRegisters->capabilities.MaxBlockLength(),
		fRegisters->capabilities.BaseClockFrequency(),
		fRegisters->capabilities.TimeoutClockFrequency());
	TRACE("Initial host control: %x\n", fRegisters->host_control.Bits());
	TRACE("Initial host control 2: %x\n", fRegisters->host_control_2);

	if (fRegisters->host_controller_version.specVersion > 3) {
		// TODO proper class for manipulating host_control_2
		fRegisters->host_control_2 &= ~(1<<12);
		TRACE("Host control 2 after disabling v4 DMA mode: %x\n", fRegisters->host_control_2);
	}

	// Turn on the power supply to the card, if there is a card inserted
	if (PowerOn()) {
		// Then we configure the clock to the frequency needed for
		// initialization
		SetClock(400, false);
	}

	fRegisters->timeout_control.SetDivider(fRegisters->capabilities.TimeoutClockFrequency(), 500);

	// Finally, configure some useful interrupts
	EnableInterrupts(SDHCI_INT_CMD_CMP | SDHCI_INT_CARD_REM
		| SDHCI_INT_TRANS_CMP | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_COMMAND_TIMEOUT);

	// We want to see the other bits in the status register, but not have an
	// interrupt trigger on them (we get a "command complete" interrupt on
	// errors already)
	fRegisters->interrupt_status_enable |= SDHCI_INT_ERROR_MASK | SDHCI_INT_NORMAL_MASK;

	if (poll) {
		// Spawn a polling thread, as the interrupts won't currently work on ACPI.
		fWorkerThread = spawn_kernel_thread(_WorkerThread, "SD bus poller",
			B_NORMAL_PRIORITY, this);
		resume_thread(fWorkerThread);
	}
}


SdhciBus::~SdhciBus()
{
	TerminateBus();

	// Stop the polling thread first: it reads the registers, which are
	// unmapped below. (This used to happen after unmapping them.)
	fStatus = B_SHUTTING_DOWN;
	status_t result;
	if (fWorkerThread > 0)
		wait_for_thread(fWorkerThread, &result);

	if (fIrq != 0)
		remove_io_interrupt_handler(fIrq, sdhci_generic_interrupt, this);

	area_id regs_area = area_for(fRegisters);
	delete_area(regs_area);

	if (fDmaArea >= 0)
		delete_area(fDmaArea);

}


void
SdhciBus::EnableInterrupts(uint32_t mask)
{
	fRegisters->interrupt_status_enable |= mask;
	fRegisters->interrupt_signal_enable |= mask;
}


void
SdhciBus::DisableInterrupts()
{
	fRegisters->interrupt_status_enable = 0;
	fRegisters->interrupt_signal_enable = 0;
}


// Status bits that end the data phase of a transfer with an error
static const uint32 kDataErrorBits = SDHCI_INT_DATA_TIMEOUT
	| SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END | SDHCI_INT_ADMA_ERROR
	| SDHCI_INT_AUTO_CMD_ERROR;

// All status bits related to data transfers, cleared before each transfer
static const uint32 kDataStatusBits = SDHCI_INT_TRANS_CMP
	| SDHCI_INT_BLOCK_GAP | SDHCI_INT_DMA | SDHCI_INT_BUF_WRITE_READY
	| SDHCI_INT_BUF_READ_READY | kDataErrorBits;

// Everything except the card insertion and removal events, which must not be
// lost when cleaning up after an error
static const uint32 kClearableStatusBits = ~(uint32)(SDHCI_INT_CARD_INS
	| SDHCI_INT_CARD_REM | SDHCI_INT_CARD_STATUS);

// #pragma mark -
/*
PartA2, SD Host Controller Simplified Specification, Version 4.20
§3.7.1.1 The sequence to issue an SD Command
*/
status_t
SdhciBus::ExecuteCommand(uint8_t command, uint32_t argument, uint32_t* response)
{
	TRACE("ExecuteCommand(%d, %x)\n", command, argument);

	// First of all clear the result
	fCommandResult = 0;

	// Check if it's possible to send a command right now.
	// It is not possible to send a command as long as the command line is busy.
	// The spec says we should wait, but we can't do that on kernel side, since
	// it leaves no chance for the upper layers to handle the problem. So we
	// just say we're busy and the caller can retry later.
	// Note that this should normally never happen: the command line is busy
	// only during command execution, and we don't leave this function with a
	// command running.
	if (fRegisters->present_state.CommandInhibit()) {
		TRACE_ALWAYS("Command execution impossible, command inhibit\n");
		return B_BUSY;
	}

	uint32_t replyType = 0;
	uint16 transferMode = 0;
	bool dataCommand = false;

	switch (command) {
		// Basic reply types
		case GO_IDLE_STATE:
			replyType = Command::kNoReplyType;
			break;
		case SD_APP_CMD:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
		case SEND_STATUS:
		case SET_BLOCK_COUNT:
			replyType = Command::kR1Type;
			break;
		case SELECT_DESELECT_CARD:
		case SD_ERASE:
		case SD_STOP_TRANSMISSION:
			replyType = Command::kR1bType;
			break;
		case ALL_SEND_CID:
		case SEND_CSD:
			replyType = Command::kR2Type;
			break;
		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND: // SD Application command
			replyType = Command::kR3Type;
			break;

		// Commands defined with different reply types in SD and MMC specifications.
		// Note that both MMC card types (standard and high capacity) must be
		// checked here, not only CARD_TYPE_MMC.
		case SD_SET_BUS_WIDTH: // SD application command. Also MMC_SWITCH, which is not.
			if (_IsMMC())
				replyType = Command::kR1bType;
			else
				replyType = Command::kR1Type;
			break;
		case SD_SEND_RELATIVE_ADDR: // also MMC_SET_RELATIVE_ADDR
			if (_IsMMC())
				replyType = Command::kR1Type;
			else
				replyType = Command::kR6Type;
			break;
		case SD_SEND_IF_COND: // also MMC_SEND_EXT_CSD
			if (_IsMMC()) {
				// On eMMC this reads the 512 byte EXT_CSD register through
				// the data lines, so only the data transfer code, which sets
				// up the buffer, can send it.
				if (!fUseDataTransferMode) {
					ERROR("MMC_SEND_EXT_CSD can only be sent through read_data\n");
					return B_NOT_ALLOWED;
				}
				dataCommand = true;
			} else
				replyType = Command::kR7Type;
			break;

		// Commands with a data phase. The transfer mode (DMA or not, stop
		// method) is decided by the data transfer code.
		case SD_READ_SINGLE_BLOCK:
		case SD_READ_MULTIPLE_BLOCKS:
		case SD_WRITE_SINGLE_BLOCK:
		case SD_WRITE_MULTIPLE_BLOCKS:
			if (!fUseDataTransferMode) {
				ERROR("Data command %d sent outside of the data transfer code\n",
					command);
				return B_NOT_ALLOWED;
			}
			dataCommand = true;
			break;
		default:
			ERROR("Unknown command %x\n", command);
			return B_BAD_DATA;
	}

	if (dataCommand) {
		replyType = Command::kR1Type | Command::kDataPresent;
		transferMode = fDataTransferMode;
	}

	// Commands that use the data lines (data transfers, and R1b commands,
	// which signal busy on DAT0) need them to be idle. CMD12 is the exception:
	// its purpose is precisely to stop a transfer that is still running.
	// Commands without a data phase (for example CMD13) can always be sent.
	//
	// Previously this returned B_BUSY right away. After a single transfer
	// that did not complete, every later command then failed and the bus
	// never recovered, so wait a bit and reset the data line instead.
	bool usesDataLine = dataCommand || replyType == Command::kR1bType;
	if (usesDataLine && command != SD_STOP_TRANSMISSION
		&& fRegisters->present_state.DataInhibit()
		&& _WaitDataLineIdle(500000) != B_OK) {
		ERROR("data line still busy before command %d (state %08" B_PRIx32
			"), resetting it\n", command, fRegisters->present_state.Bits());
		fRegisters->software_reset.ResetDataLine();
	}

	if (fRegisters->present_state.CommandInhibit())
		panic("Command line busy at start of execute command\n");

	// Get ready to accept interrupts that will occur during the command
	ConditionVariableEntry waiter;
	fInterruptNotifier.Add(&waiter);

	fRegisters->argument = argument;

	if (replyType == Command::kR1bType || dataCommand)
		fRegisters->transfer_mode = transferMode;

	fRegisters->command.SendCommand(command, replyType);

	// Wait for command response to be available ("command complete" interrupt)
	TRACE("Wait for command complete...");
	int rounds = 0;
	do {
		status_t result = waiter.Wait(B_RELATIVE_TIMEOUT, 1000000);
		if (result == B_TIMED_OUT) {
			TRACE("Command complete interrupt did not trigger for a while, status %x\n",
				fRegisters->interrupt_status);
			if (++rounds >= 5 && fCommandResult == 0) {
				ERROR("no response to command %d after 5s (int status %08"
					B_PRIx32 ", state %08" B_PRIx32 "), resetting\n", command,
					fRegisters->interrupt_status,
					fRegisters->present_state.Bits());
				fRegisters->software_reset.ResetCommandAndDataLines();
				return B_TIMED_OUT;
			}
		} else if (result != B_OK)
			panic("sdhci: Failed to wait for command complete: %s", strerror(result));

		fInterruptNotifier.Add(&waiter);
		TRACE("Command status: %x\n", fCommandResult);
		TRACE("real status = %x command line busy: %d\n",
			fRegisters->interrupt_status,
			fRegisters->present_state.CommandInhibit());
	} while (fCommandResult == 0);

	TRACE("Command response available\n");

	if (fCommandResult & SDHCI_INT_ERROR) {
		// TODO is it a good idea to clear interrupts here from outside the interrupt handler?
		// Write-1-to-clear register: write only the bits to acknowledge. A
		// read-modify-write ("|=") would also clear bits that arrived in the
		// meantime, without anybody having seen them.
		fRegisters->interrupt_status = fCommandResult & kClearableStatusBits;
		if (fCommandResult & SDHCI_INT_COMMAND_TIMEOUT) {
			ERROR("Command execution timed out\n");
			// At this point, the "command inhibit" bit is not set yet, it will be set only after
			// another command is sent while the controller is in the timeout state.
			// But resetting the controller state pre-emptively will allow to send another command.
			//
			// Clear the data line at the same time if it is busy
			fRegisters->software_reset.ResetCommandAndDataLines();
			return B_TIMED_OUT;
		}
		if (fCommandResult & SDHCI_INT_COMMAND_CRC) {
			ERROR("CRC error\n");
			return B_BAD_VALUE;
		}
		ERROR("Command execution failed %x\n", fCommandResult);
		// TODO look at errors in interrupt_status register for more details
		// and return a more appropriate error code
		return B_ERROR;
	}

	if (fRegisters->present_state.CommandInhibit()) {
		TRACE("Command execution failed, card stalled\n");
		// Clear the stall
		fRegisters->software_reset.ResetCommandLine();
		return B_ERROR;
	}

	switch (replyType & Command::kReplySizeMask) {
		case Command::k32BitResponse:
			*response = fRegisters->response[0];
			break;
		case Command::k128BitResponse:
			response[0] = fRegisters->response[0];
			response[1] = fRegisters->response[1];
			response[2] = fRegisters->response[2];
			response[3] = fRegisters->response[3];
			break;

		default:
			// No response
			break;
	}

	if ((replyType == Command::kR1bType)
			&& (fCommandResult & SDHCI_INT_TRANSFER_MASK) == 0) {
		// R1b commands may use the data line so we must wait for the busy
		// signal on DAT0 to end. This used to wait forever, which hung the
		// whole bus if the card never released the line.
		TRACE("Waiting for data line...\n");
		if (_WaitDataLineIdle(2000000) != B_OK) {
			ERROR("busy signal after command %d did not end (state %08"
				B_PRIx32 "), resetting the data line\n", command,
				fRegisters->present_state.Bits());
			fRegisters->software_reset.ResetDataLine();
			return B_TIMED_OUT;
		}
		TRACE("Dataline is released.\n");
	}

	// Remember which card is selected, it is needed to query its status
	// during error recovery
	if (command == SELECT_DESELECT_CARD)
		fSelectedRca = argument >> 16;

	TRACE("Command execution %d complete\n", command);
	return B_OK;
}


status_t
SdhciBus::InitCheck()
{
	return fStatus;
}


void
SdhciBus::Reset()
{
	if (!fRegisters->software_reset.ResetAll())
		ERROR("SdhciBus::Reset: SoftwareReset timeout\n");
}


void
SdhciBus::SetClock(int kilohertz, bool allowAuto)
{
	// Preset values only choose the divider: the clock must already be
	// running. It is not when the slot was empty at startup and a card was
	// inserted later, so set it explicitly then.
	if (allowAuto && (fRegisters->host_controller_version.specVersion > 2)
		&& fRegisters->clock_control.SDEnabled()) {
		TRACE("Ignoring set_clock, controller support presets\n");
		fRegisters->host_control_2 |= (1<<15);
		TRACE("Host control 2 after enabling preset mode: %x\n", fRegisters->host_control_2);
		return;
	}

	int base_clock = fRegisters->capabilities.BaseClockFrequency();
	// Try to get as close to the requested frequency as possible, but not
	// faster: round the divider up. (It used to be rounded down, which went
	// unnoticed for 400kHz and 25MHz, which divide the usual base clocks
	// exactly, but made a 52MHz request run at 100MHz on a 200MHz base.)
	int divider = (base_clock * 1000 + kilohertz - 1) / kilohertz;

	// The clock to the card must be stopped while the divider changes
	fRegisters->clock_control.DisableSD();

	if (fRegisters->host_controller_version.specVersion <= 1) {
		// Old controller only support power of two dividers up to 256,
		// round to next power of two up to 256
		if (divider > 256)
			divider = 256;

		divider--;
		divider |= divider >> 1;
		divider |= divider >> 2;
		divider |= divider >> 4;
		divider++;
	}

	divider = fRegisters->clock_control.SetDivider(divider);

	// Log the value after possible rounding by SetDivider (only even values
	// are allowed).
	TRACE("SDCLK frequency: requested %dkHz, effective %dMHz / %d = %dkHz\n", kilohertz,
		base_clock, divider, base_clock * 1000 / divider);

	// We have set the divider, now we can enable the internal clock.
	fRegisters->clock_control.EnableInternal();

	// wait until internal clock is stabilized
	while (!(fRegisters->clock_control.InternalStable()));

	fRegisters->clock_control.EnablePLL();
	while (!(fRegisters->clock_control.InternalStable()));

	// Finally, route the clock to the SD card
	fRegisters->clock_control.EnableSD();
}


// #pragma mark - data transfers


static const uint32 kBlockSize = 512;

// Long enough for 512KB on a single data line at 25MHz (about 170ms), with a
// large margin for slow devices and writes.
static const bigtime_t kTransferTimeout = 5000000;

// Error bits of the R1 card status that mean the card refused a command
static const uint32 kCardStatusErrorBits = (1u << 31) | (1 << 30) | (1 << 29)
	| (1 << 28) | (1 << 27) | (1 << 26) | (1 << 24) | (1 << 23) | (1 << 22)
	| (1 << 21) | (1 << 20) | (1 << 19) | (1 << 7);

static const char* const kDataModeNames[] = { "sdma", "pio" };
static const char* const kStopModeNames[] = { "cmd23", "auto_cmd12", "single" };


/*!	Slot type 1 in the capabilities: a device soldered to the board, such as
	an eMMC. It is always there, card detection means nothing for it.
*/
bool
SdhciBus::_IsEmbeddedSlot()
{
	return fRegisters->capabilities.SlotType() == 1;
}


bool
SdhciBus::_IsCardPresent()
{
	return _IsEmbeddedSlot() || fRegisters->present_state.IsCardInserted();
}


bool
SdhciBus::_UsesSectorAddressing() const
{
	// Standard capacity SD cards and MMC devices up to 2GB use byte offsets,
	// all the other types use sector offsets.
	return fCardType != CARD_TYPE_SD && fCardType != CARD_TYPE_MMC;
}


status_t
SdhciBus::_WaitDataLineIdle(bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	bigtime_t delay = 5;
	while (fRegisters->present_state.DataInhibit()) {
		if (system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(delay);
		if (delay < 1000)
			delay *= 2;
	}
	return B_OK;
}


status_t
SdhciBus::_SendDataCommand(uint8 command, uint32 argument,
	uint16 transferMode, uint32* _response)
{
	fUseDataTransferMode = true;
	fDataTransferMode = transferMode;
	status_t status = ExecuteCommand(command, argument, _response);
	fUseDataTransferMode = false;
	return status;
}


void
SdhciBus::_LogCardStatus(const char* when, uint32 status)
{
	static const char* const kStates[] = { "idle", "ready", "ident", "stby",
		"tran", "data", "rcv", "prg", "dis", "btst", "slp", "?", "?", "?", "?",
		"?" };

	ERROR("card status %s: %08" B_PRIx32 " state=%s ready_for_data=%d"
		"%s%s%s%s%s%s%s%s%s\n", when, status, kStates[(status >> 9) & 0xF],
		(int)((status >> 8) & 1),
		(status & (1u << 31)) != 0 ? " ADDRESS_OUT_OF_RANGE" : "",
		(status & (1 << 30)) != 0 ? " ADDRESS_MISALIGN" : "",
		(status & (1 << 29)) != 0 ? " BLOCK_LEN_ERROR" : "",
		(status & (1 << 26)) != 0 ? " WP_VIOLATION" : "",
		(status & (1 << 23)) != 0 ? " COM_CRC_ERROR" : "",
		(status & (1 << 22)) != 0 ? " ILLEGAL_COMMAND" : "",
		(status & (1 << 21)) != 0 ? " DEVICE_ECC_FAILED" : "",
		(status & (1 << 19)) != 0 ? " ERROR" : "",
		(status & (1 << 7)) != 0 ? " SWITCH_ERROR" : "");
}


/*!	Brings controller and card back to a known state after a transfer that
	did not complete. Without this, the card stays in the data state (for
	example still streaming an open ended multiple block read), DAT0 stays
	low, and every following command fails.
*/
void
SdhciBus::_AbortTransfer(const char* reason)
{
	ERROR("abort (%s): state %08" B_PRIx32 ", int status %08" B_PRIx32
		", adma error %02x, auto cmd error %04x, host control %02x\n", reason,
		fRegisters->present_state.Bits(), fRegisters->interrupt_status,
		fRegisters->adma_error_status, fRegisters->auto_cmd12_error_status,
		fRegisters->host_control.Bits());

	// Stop the controller side of the transfer
	fRegisters->software_reset.ResetCommandAndDataLines();
	fRegisters->interrupt_status = kClearableStatusBits;

	// Ask the card where it is. CMD12 is only needed if it is still sending
	// or receiving data: in the transfer state it is an illegal command, which
	// then shows up as ILLEGAL_COMMAND in the next status.
	uint32 response = 0;
	status_t status;
	bool needStop = true;
	if (fSelectedRca != 0) {
		status = ExecuteCommand(SEND_STATUS, (uint32)fSelectedRca << 16,
			&response);
		if (status == B_OK) {
			_LogCardStatus("at abort", response);
			const uint32 state = (response >> 9) & 0xF;
			const uint32 kStateData = 5;
			const uint32 kStateReceive = 6;
			needStop = state == kStateData || state == kStateReceive;
		} else
			ERROR("abort: CMD13 failed: %s\n", strerror(status));
	}

	if (needStop) {
		status = ExecuteCommand(SD_STOP_TRANSMISSION, 0, &response);
		TRACE("abort: CMD12 %s\n", strerror(status));
		if (fSelectedRca != 0
			&& ExecuteCommand(SEND_STATUS, (uint32)fSelectedRca << 16,
				&response) == B_OK) {
			_LogCardStatus("after CMD12", response);
		}
	}

	if (fRegisters->present_state.DataInhibit()
		&& _WaitDataLineIdle(500000) != B_OK) {
		fRegisters->software_reset.ResetDataLine();
	}
	fRegisters->interrupt_status = kClearableStatusBits;
}


/*!	Runs one data command and its data phase.
	\a physical is used for DMA. For PIO, the data is copied from/to \a buffer
	if it is not NULL (kernel address), and from/to \a physical otherwise.
*/
status_t
SdhciBus::_TransferChunk(uint8 command, uint32 argument, uint32 blockCount,
	uint32 blockSize, bool isWrite, int dataMode, int stopMode,
	phys_addr_t physical, uint8* buffer, bigtime_t timeout)
{
	const bool multi = command == SD_READ_MULTIPLE_BLOCKS
		|| command == SD_WRITE_MULTIPLE_BLOCKS;
	const size_t length = (size_t)blockCount * blockSize;

	if (blockSize == 0 || blockSize > kBlockSize || (blockSize % 4) != 0
		|| blockCount == 0 || blockCount > 0xffff) {
		return B_BAD_VALUE;
	}

	if (fRegisters->present_state.DataInhibit()
		&& _WaitDataLineIdle(timeout) != B_OK) {
		_AbortTransfer("data line busy before transfer");
	}

	// Forget status bits left over by an earlier transfer
	fRegisters->interrupt_status = kDataStatusBits;

	// (The casts avoid binding a reference to the in-class constants, which
	// have no out-of-class definition.)
	uint16 transferMode = isWrite
		? (uint16)TransferMode::kWrite : (uint16)TransferMode::kRead;
	if (multi) {
		transferMode |= TransferMode::kMulti | TransferMode::kBlockCountEnable;
		if (stopMode == kStopModeAutoCmd12)
			transferMode |= TransferMode::kAutoCmd12Enable;
	}

	if (dataMode != kDataModePio && (uint64)physical + length > 0x100000000ULL) {
		ERROR("buffer at %" B_PRIxPHYSADDR " is above 4GB, cannot use DMA\n",
			physical);
		return B_BAD_ADDRESS;
	}

	switch (dataMode) {
		case kDataModeSdma:
			fRegisters->host_control.SetDMAMode(HostControl::kSdma);
			fRegisters->system_address = (uint32)physical;
			transferMode |= TransferMode::kDmaEnable;
			break;

		default:
			// PIO, the CPU moves the data through the buffer data port
			break;
	}

	// For multiple block transfers, the DMA boundary is set to the largest
	// value. The device node properties make sure the DMA resource never gives
	// us a buffer crossing it, but SDMA boundary interrupts are handled below
	// anyway.
	fRegisters->block_size.ConfigureTransfer(blockSize,
		BlockSize::kDmaBoundary512K);
	fRegisters->block_count = blockCount;

	uint32 response = 0;
	status_t status = _SendDataCommand(command, argument, transferMode,
		&response);
	if (status != B_OK) {
		TRACE("data command %u (argument %" B_PRIx32 ", %" B_PRIu32
			" blocks, %s/%s) failed: %s\n", command, argument, blockCount,
			kDataModeNames[dataMode], kStopModeNames[stopMode],
			strerror(status));
		_AbortTransfer("data command failed");
		return status;
	}

	if ((response & kCardStatusErrorBits) != 0)
		_LogCardStatus("in response to the data command", response);

	uint32 intStatus = 0;

	if (dataMode == kDataModePio) {
		const uint32 readyBit = isWrite
			? SDHCI_INT_BUF_WRITE_READY : SDHCI_INT_BUF_READ_READY;
		const uint32 wordCount = blockSize / 4;
		uint32 words[kBlockSize / 4];

		// Present state: buffer write enable (bit 10), buffer read enable
		// (bit 11). They say the same as the buffer ready status bits, but
		// cannot be lost by a status acknowledge, so check both.
		const uint32 bufferEnableBit = isWrite ? (1 << 10) : (1 << 11);

		for (uint32 block = 0; block < blockCount; block++) {
			bigtime_t deadline = system_time() + timeout;
			bigtime_t delay = 5;
			while (true) {
				uint32 live = fRegisters->interrupt_status;
				uint32 recorded = fCommandResult;
				intStatus = live | (recorded & kDataErrorBits);
				if ((intStatus & kDataErrorBits) != 0) {
					status = B_IO_ERROR;
					break;
				}
				if ((live & readyBit) != 0
					|| (fRegisters->present_state.Bits() & bufferEnableBit) != 0)
					break;
				if (system_time() >= deadline) {
					status = B_TIMED_OUT;
					break;
				}
				snooze(delay);
				if (delay < 1000)
					delay *= 2;
			}
			if (status != B_OK)
				break;

			fRegisters->interrupt_status = readyBit;

			size_t offset = (size_t)block * blockSize;
			if (isWrite) {
				if (buffer != NULL)
					memcpy(words, buffer + offset, blockSize);
				else
					vm_memcpy_from_physical(words, physical + offset, blockSize,
						false);
				for (uint32 i = 0; i < wordCount; i++)
					fRegisters->buffer_data_port = words[i];
			} else {
				for (uint32 i = 0; i < wordCount; i++)
					words[i] = fRegisters->buffer_data_port;
				if (buffer != NULL)
					memcpy(buffer + offset, words, blockSize);
				else
					vm_memcpy_to_physical(physical + offset, words, blockSize,
						false);
			}
		}
	}

	if (status == B_OK) {
		// Wait for the end of the transfer. The transfer complete status bit
		// is the normal signal. As a safety net, the end of the data line
		// busy state means the same (the controller clears both together),
		// and cannot be lost. SDMA stops at each buffer boundary, writing the
		// address register again lets it continue.
		static int32 sFallbackLogCount = 0;
		bigtime_t deadline = system_time() + timeout;
		while (true) {
			uint32 live = fRegisters->interrupt_status;
			uint32 recorded = fCommandResult;
			intStatus = live | (recorded & (SDHCI_INT_TRANS_CMP | kDataErrorBits));

			if ((intStatus & kDataErrorBits) != 0) {
				status = B_IO_ERROR;
				break;
			}
			if ((intStatus & SDHCI_INT_TRANS_CMP) != 0)
				break;
			if ((live & SDHCI_INT_DMA) != 0) {
				fRegisters->interrupt_status = SDHCI_INT_DMA;
				fRegisters->system_address = fRegisters->system_address;
				continue;
			}
			if (!fRegisters->present_state.DataInhibit()) {
				// The controller sets the status bit and releases the data
				// line together, so the bit may simply have arrived between
				// the two register reads above. Check once more.
				uint32 again = fRegisters->interrupt_status | fCommandResult;
				if ((again & kDataErrorBits) != 0) {
					status = B_IO_ERROR;
					break;
				}
				if ((again & SDHCI_INT_TRANS_CMP) != 0)
					break;
				if (atomic_add(&sFallbackLogCount, 1) < 5) {
					TRACE("transfer end detected from the data line state, "
						"transfer complete status not seen (command %u)\n",
						command);
				}
				break;
			}
			if (system_time() >= deadline) {
				status = B_TIMED_OUT;
				break;
			}

			// Sleep until the next interrupt: transfer complete is signaled,
			// and the interrupt handler wakes us up right away. This used to
			// poll with sleeps growing up to 1ms, which at 200MHz (about
			// 2.5ms for 512KB) wasted a large part of the time. The timeout
			// only matters for the data line check above, which comes without
			// an interrupt.
			ConditionVariableEntry waiter;
			fInterruptNotifier.Add(&waiter);
			if (((fRegisters->interrupt_status | fCommandResult)
					& (SDHCI_INT_TRANS_CMP | kDataErrorBits | SDHCI_INT_DMA))
					== 0) {
				waiter.Wait(B_RELATIVE_TIMEOUT, 1000);
			}
		}
	}

	if (status != B_OK) {
		ERROR("transfer failed: command %u, argument %" B_PRIx32 ", %" B_PRIu32
			" blocks, %s/%s: %s, int status %08" B_PRIx32 ", state %08" B_PRIx32
			", auto cmd error %04x\n", command, argument, blockCount,
			kDataModeNames[dataMode], kStopModeNames[stopMode],
			strerror(status), intStatus, fRegisters->present_state.Bits(),
			fRegisters->auto_cmd12_error_status);
		_AbortTransfer("data phase failed");
		return status;
	}

	// The card may keep DAT0 low while it finishes internally, after a write
	// or after the stop command sent by auto CMD12. Wait for that, so that the
	// next command does not find the data line busy.
	if (_WaitDataLineIdle(timeout) != B_OK) {
		_AbortTransfer("data line stuck busy after transfer");
		return B_TIMED_OUT;
	}

	fRegisters->interrupt_status = kDataStatusBits;
	return B_OK;
}


status_t
SdhciBus::_TransferBlocks(bool isWrite, uint32 argument, uint32 argumentStep,
	uint32 blockCount, int dataMode, int stopMode, phys_addr_t physical,
	uint8* buffer, bigtime_t timeout)
{
	if (stopMode == kStopModeSingle) {
		uint8 command = isWrite ? SD_WRITE_SINGLE_BLOCK : SD_READ_SINGLE_BLOCK;
		for (uint32 i = 0; i < blockCount; i++) {
			status_t status = _TransferChunk(command, argument + i * argumentStep,
				1, kBlockSize, isWrite, dataMode, stopMode,
				physical + (phys_addr_t)i * kBlockSize,
				buffer != NULL ? buffer + (size_t)i * kBlockSize : NULL,
				timeout);
			if (status != B_OK)
				return status;
		}
		return B_OK;
	}

	if (stopMode == kStopModeCmd23) {
		// Bit 31 of the argument would request a reliable write, we don't
		// want that.
		uint32 response = 0;
		status_t status = ExecuteCommand(SET_BLOCK_COUNT, blockCount & 0xffff,
			&response);
		if (status != B_OK) {
			ERROR("CMD23 (set block count %" B_PRIu32 ") failed: %s\n",
				blockCount, strerror(status));
			return status;
		}
		if ((response & kCardStatusErrorBits) != 0)
			_LogCardStatus("in response to CMD23", response);
	}

	uint8 command = isWrite ? SD_WRITE_MULTIPLE_BLOCKS : SD_READ_MULTIPLE_BLOCKS;
	return _TransferChunk(command, argument, blockCount, kBlockSize, isWrite,
		dataMode, stopMode, physical, buffer, timeout);
}


/*!	Decides how data is moved, the first time data is accessed: simple DMA
	when the controller has it, PIO otherwise. eMMC devices always support
	CMD23 (set block count); for SD cards, where it is optional, the controller
	sends CMD12 by itself at the end of a transfer (auto CMD12).
*/
status_t
SdhciBus::_SelectTransferMode()
{
	if (fTransferModeSelected)
		return B_OK;
	fTransferModeSelected = true;

	const bool canSdma = fRegisters->capabilities.SimpleDMA();
	TRACE_ALWAYS("controller: spec %d, SDMA %s, high speed %s, DDR50 %s, "
		"SDR104/HS200 %s, base clock %u MHz\n",
		fRegisters->host_controller_version.specVersion,
		canSdma ? "yes" : "no",
		fRegisters->capabilities.HighSpeed() ? "yes" : "no",
		fRegisters->capabilities.DDR50Support() ? "yes" : "no",
		fRegisters->capabilities.SDR104Support() ? "yes" : "no",
		fRegisters->capabilities.BaseClockFrequency());

	fDataMode = canSdma ? kDataModeSdma : kDataModePio;
	fStopMode = _IsMMC() ? kStopModeCmd23 : kStopModeAutoCmd12;

	TRACE_ALWAYS("data transfers: %s, %s\n", kDataModeNames[fDataMode],
		kStopModeNames[fStopMode]);
	return B_OK;
}


status_t
SdhciBus::DoIO(uint8_t command, IOOperation* operation, bool offsetAsSectors)
{
	// The command given by the upper layer is only a hint (read or write
	// multiple blocks): the actual commands depend on the transfer method.
	(void)command;

	bool isWrite = operation->IsWrite();
	off_t offset = operation->Offset();
	generic_size_t length = operation->Length();

	TRACE("%s %" B_PRIuGENADDR " bytes at %" B_PRIdOFF "\n",
		isWrite ? "Write" : "Read", length, offset);

	// Check that the IO scheduler did its job in following our DMA restrictions
	// We can start a read only at a sector boundary
	ASSERT(offset % kBlockSize == 0);
	// We can only read complete sectors
	ASSERT(length % kBlockSize == 0);

	_SelectTransferMode();

	const generic_io_vec* vecs = operation->Vecs();
	generic_size_t vecOffset = 0;

	while (length > 0) {
		size_t toCopy = std::min((generic_size_t)length,
			vecs->length - vecOffset);

		// If the current vec is empty, we can move to the next
		if (toCopy == 0) {
			vecs++;
			vecOffset = 0;
			continue;
		}

		ASSERT(toCopy % kBlockSize == 0);

		uint32 argument = offset / (offsetAsSectors ? kBlockSize : 1);
		status_t status = _TransferBlocks(isWrite, argument,
			offsetAsSectors ? 1 : kBlockSize, toCopy / kBlockSize, fDataMode,
			fStopMode, vecs->base + vecOffset, NULL, kTransferTimeout);
		if (status != B_OK) {
			ERROR("%s of %" B_PRIuSIZE " bytes at %" B_PRIdOFF " failed: %s\n",
				isWrite ? "write" : "read", toCopy, offset, strerror(status));
			return status;
		}

		length -= toCopy;
		vecOffset += toCopy;
		offset += toCopy;
	}

	return B_OK;
}


status_t
SdhciBus::ReadData(uint8_t command, uint32_t argument, void* buffer,
	size_t length)
{
	if (buffer == NULL || length == 0 || length > kBlockSize
		|| (length % 4) != 0) {
		return B_BAD_VALUE;
	}

	_SelectTransferMode();

	int dataMode = fDataMode;
	status_t status = _TransferChunk(command, argument, 1, length, false,
		dataMode, kStopModeSingle, fBouncePhysical,
		dataMode == kDataModePio ? fBounceBuffer : NULL, kTransferTimeout);
	if (status != B_OK) {
		ERROR("read_data(command %u, %" B_PRIuSIZE " bytes) failed: %s\n",
			command, length, strerror(status));
		return status;
	}

	memcpy(buffer, fBounceBuffer, length);
	return B_OK;
}


void
SdhciBus::SetScanSemaphore(sem_id sem)
{
	fScanSemaphore = sem;

	// If there is already a card in, start a scan immediately. A device
	// soldered to the board (embedded slot) is always there, whatever the card
	// detect bit says: it may not be stable yet right after the reset.
	if (_IsCardPresent())
		release_sem(fScanSemaphore);

	TRACE("slot type %d (%s), card detect %s\n",
		fRegisters->capabilities.SlotType(),
		_IsEmbeddedSlot() ? "embedded, card detect ignored" : "removable",
		fRegisters->present_state.IsCardInserted() ? "inserted" : "empty");

	// We can now enable the card insertion interrupt for next time a card
	// is inserted. Not for an embedded slot: insertion and removal events
	// mean nothing there, and acting on them only disturbs the device.
	if (!_IsEmbeddedSlot())
		EnableInterrupts(SDHCI_INT_CARD_INS);
}


void
SdhciBus::SetBusWidth(int width)
{
	uint8_t widthBits;
	switch(width) {
		case 1:
			widthBits = HostControl::kDataTransfer1Bit;
			break;
		case 4:
			widthBits = HostControl::kDataTransfer4Bit;
			break;
		case 8:
			widthBits = HostControl::kDataTransfer8Bit;
			break;
		default:
			panic("Incorrect bitwidth value");
			return;
	}
	fRegisters->host_control.SetDataTransferWidth(widthBits);
}


void
SdhciBus::SetCardType(card_type type)
{
	fCardType = type;
}


bool
SdhciBus::PowerOn()
{
	if (!_IsCardPresent()) {
		TRACE("Card not inserted, not powering on for now\n");
		return false;
	}

	uint8_t supportedVoltages = fRegisters->capabilities.SupportedVoltages();
	if ((supportedVoltages & Capabilities::k3v3) != 0)
		fRegisters->power_control.SetVoltage(PowerControl::k3v3);
	else if ((supportedVoltages & Capabilities::k3v0) != 0)
		fRegisters->power_control.SetVoltage(PowerControl::k3v0);
	else if ((supportedVoltages & Capabilities::k1v8) != 0)
		fRegisters->power_control.SetVoltage(PowerControl::k1v8);
	else {
		fRegisters->power_control.PowerOff();
		ERROR("No voltage is supported\n");
		return false;
	}

	return true;
}


void
SdhciBus::PowerOff()
{
	fRegisters->power_control.PowerOff();
}


void
SdhciBus::TerminateBus()
{
	CALLED();

	DisableInterrupts();
	fRegisters->clock_control.DisableSD();
	PowerOff();
	/*
	// Debugging.
	uint8_t powerBits = fRegisters->power_control.Bits();
	uint16_t clockBits = fRegisters->clock_control.Bits();
	if ((powerBits & 0x1) != 0 || (clockBits & (1 << 2)) != 0) {
		ERROR("TerminateBus: Not killed. "
			"(power=%#x, clock=%#x)\n", powerBits, clockBits);
	} else {
		TRACE("TerminateBus: killed. (power=%#x, "
			"clock=%#x)\n", powerBits, clockBits);
	}
	*/
}


void
SdhciBus::RecoverError()
{
	fRegisters->interrupt_signal_enable &= ~(SDHCI_INT_CMD_CMP
		| SDHCI_INT_TRANS_CMP | SDHCI_INT_CARD_INS | SDHCI_INT_CARD_REM);

	if (fRegisters->interrupt_status & 7)
		fRegisters->software_reset.ResetCommandLine();

	int16_t error_status = fRegisters->interrupt_status;
	fRegisters->interrupt_status &= ~(error_status);
}


int32
SdhciBus::HandleInterrupt()
{
#if 0
	// We could use the slot register to quickly see for which slot the
	// interrupt is. But since we have an interrupt handler call for each slot
	// anyway, it's just as simple to let each of them scan its own interrupt
	// status register.
	if ( !(fRegisters->slot_interrupt_status & (1 << fSlot)) ) {
		TRACE("interrupt not for me.\n");
		return B_UNHANDLED_INTERRUPT;
	}
#endif
	
	uint32_t intmask = fRegisters->interrupt_status;

	// Shortcut: exit early if there is no interrupt or if the register is
	// clearly invalid.
	if ((intmask == 0) || (intmask == 0xffffffff)) {
		return B_UNHANDLED_INTERRUPT;
	}

	TRACE("interrupt function called %x\n", intmask);

	// handling card presence interrupts
	if ((intmask & SDHCI_INT_CARD_REM) != 0) {
		// We can get spurious interrupts as the card is inserted or removed,
		// so check the actual state before acting
		// Never power off an embedded device on such an event
		if (!_IsEmbeddedSlot() && !fRegisters->present_state.IsCardInserted())
			fRegisters->power_control.PowerOff();
		else
			TRACE("Card removed interrupt ignored\n");

		fRegisters->interrupt_status = SDHCI_INT_CARD_REM;
		TRACE("Card removal interrupt handled\n");
	}

	if ((intmask & SDHCI_INT_CARD_INS) != 0) {
		// We can get spurious interrupts as the card is inserted or removed,
		// so check the actual state before acting. The clock is not touched
		// here any more: the scan sets it itself, and changing it from the
		// interrupt handler can corrupt a command the scan is sending at the
		// same moment (a corrupted CMD0 left an eMMC device un-reset, see
		// mmc_bus). Embedded slots ignore these events altogether.
		if (!_IsEmbeddedSlot() && fRegisters->present_state.IsCardInserted()) {
			PowerOn();
			release_sem_etc(fScanSemaphore, 1, B_DO_NOT_RESCHEDULE);
		} else
			TRACE("Card insertion interrupt ignored\n");

		fRegisters->interrupt_status = SDHCI_INT_CARD_INS;
		TRACE("Card presence interrupt handled\n");
	}

	// handling command interrupt
	if (intmask & SDHCI_INT_CMD_MASK) {
		fCommandResult |= intmask;
			// Save the status before clearing so the thread can handle it

		// The status register is write-1-to-clear: write only the bits seen
		// above. It used to be updated with "|=", a read-modify-write that
		// also cleared any bit set since intmask was read (for example the
		// transfer complete or buffer ready status of a short transfer),
		// without recording it anywhere. The transfer then looked like it
		// never finished.
		fRegisters->interrupt_status = (intmask & SDHCI_INT_CMD_MASK);

		// Notify the thread
		fInterruptNotifier.NotifyAll();
		TRACE("Command complete interrupt handled\n");
	}

	if (intmask & SDHCI_INT_TRANSFER_MASK) {
		fCommandResult |= intmask;
		fRegisters->interrupt_status = (intmask & SDHCI_INT_TRANSFER_MASK);
		fInterruptNotifier.NotifyAll();
		TRACE("Transfer complete interrupt handled\n");
	}

	// handling bus power interrupt
	if (intmask & SDHCI_INT_BUS_POWER) {
		fRegisters->interrupt_status = SDHCI_INT_BUS_POWER;
		TRACE("card is consuming too much power\n");
	}

	// Check that all interrupts have been cleared (we check all the ones we
	// enabled, so that should always be the case)
	intmask = fRegisters->interrupt_status;
	if (intmask != 0) {
		// Bits that arrived while the handler was running are left for the
		// next interrupt (or for the thread polling them), this is expected
		TRACE("Remaining interrupts at end of handler: %x\n", intmask);
	}

	return B_HANDLED_INTERRUPT;
}


status_t
SdhciBus::_WorkerThread(void* cookie) {
	SdhciBus* bus = (SdhciBus*)cookie;
	bigtime_t delay = 50;
	while (bus->fStatus != B_SHUTTING_DOWN) {
		// Exactly what the interrupt handler would do. Checking only for
		// command and transfer complete, as before, missed errors such as a
		// command timeout, so every probe that timed out waited 5 seconds.
		// Poll quickly while there is activity, back off to 1ms when idle.
		if (bus->HandleInterrupt() == B_HANDLED_INTERRUPT)
			delay = 50;
		else if (delay < 1000)
			delay += 50;
		snooze(delay);
	}
	TRACE("poller thread terminating");
	return B_OK;
}


// #pragma mark -


void
uninit_bus(void* bus_cookie)
{
	SdhciBus* bus = (SdhciBus*)bus_cookie;
	delete bus;

	// FIXME do we need to put() the PCI module here?
}


void
bus_removed(void* bus_cookie)
{
	return;
}


static status_t
register_child_devices(void* cookie)
{
	CALLED();
	SdhciDevice* context = (SdhciDevice*)cookie;
	status_t status = B_OK;
	const char* bus;
	device_node* parent = gDeviceManager->get_parent_node(context->fNode);
	status = gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false);
	if (status != B_OK) {
		TRACE("Could not find required attribute device/bus\n");
		return status;
	}

	if (strcmp(bus, "pci") == 0)
		status = register_child_devices_pci(cookie);
	else if (strcmp(bus, "acpi") == 0)
		status = register_child_devices_acpi(cookie);
	else
		status = B_BAD_VALUE;

	return status;
}


static status_t
init_device(device_node* node, void** device_cookie)
{
	CALLED();

	SdhciDevice* context = new(std::nothrow)SdhciDevice;
	if (context == NULL)
		return B_NO_MEMORY;
	context->fNode = node;
	*device_cookie = context;

	status_t status = B_OK;
	const char* bus;
	device_node* parent = gDeviceManager->get_parent_node(node);
	status = gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false);
	if (status != B_OK) {
		TRACE("Could not find required attribute device/bus\n");
		return status;
	}

	if (strcmp(bus, "pci") == 0)
		return init_device_pci(node, context);

	return B_OK;
}


static void
uninit_device(void* device_cookie)
{
	SdhciDevice* context = (SdhciDevice*)device_cookie;
	device_node* parent = gDeviceManager->get_parent_node(context->fNode);

	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK) {
		TRACE("Could not find required attribute device/bus\n");
	}

	if (strcmp(bus, "pci") == 0)
		uninit_device_pci(context, parent);

	gDeviceManager->put_node(parent);

	delete context;
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "SD Host Controller"}},
		{}
	};

	return gDeviceManager->register_node(parent, SDHCI_DEVICE_MODULE_NAME,
		attrs, NULL, NULL);
}


static float
supports_device(device_node* parent)
{
	const char* bus;

	// make sure parent is either an ACPI or PCI SDHCI device node
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
		!= B_OK) {
		TRACE("Could not find required attribute device/bus\n");
		return -1;
	}

	if (strcmp(bus, "pci") == 0)
		return supports_device_pci(parent);
	else if (strcmp(bus, "acpi") == 0)
		return supports_device_acpi(parent);

	return 0.0f;
}


module_dependency module_dependencies[] = {
	{ MMC_BUS_MODULE_NAME, (module_info**)&gMMCBusController},
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};

status_t
set_clock(void* controller, uint32_t kilohertz)
{
	SdhciBus* bus = (SdhciBus*)controller;

	bus->SetClock(kilohertz, true);
	return B_OK;
}


status_t
execute_command(void* controller, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->ExecuteCommand(command, argument, response);
}


status_t
do_io(void* controller, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->DoIO(command, operation, offsetAsSectors);
}


void
set_scan_semaphore(void* controller, sem_id sem)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->SetScanSemaphore(sem);
}


void
set_bus_width(void* controller, int width)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->SetBusWidth(width);
}


void
set_card_type(void* controller, card_type type)
{
	SdhciBus* bus = (SdhciBus*)controller;
	bus->SetCardType(type);
}


void
terminate_bus(void* controller)
{
	SdhciBus* bus = (SdhciBus*)controller;
	bus->TerminateBus();
}


status_t
read_data(void* controller, uint8_t command, uint32_t argument, void* buffer,
	size_t length)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->ReadData(command, argument, buffer, length);
}


// Root device that binds to the ACPI or PCI bus. It will register an mmc_bus_interface
// node for each SD slot in the device.
static driver_module_info sSDHCIDevice = {
	{
		SDHCI_DEVICE_MODULE_NAME,
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


module_info* modules[] = {
	(module_info* )&sSDHCIDevice,
	(module_info* )&gSDHCIPCIDeviceModule,
	(module_info* )&gSDHCIACPIDeviceModule,
	NULL
};
