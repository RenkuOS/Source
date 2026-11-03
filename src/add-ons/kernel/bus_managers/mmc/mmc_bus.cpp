/*
 * Copyright 2018-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 */
#include "mmc_bus.h"

#include <Errors.h>

#include <stdint.h>


MMCBus::MMCBus(device_node* node)
	:
	fNode(node),
	fController(NULL),
	fCookie(NULL),
	fStatus(B_OK),
	fWorkerThread(-1),
	fInitialScanDone(-1),
	fInitialScanRequested(false),
	fInitialScanSignaled(false),
	fInitialScanWaited(false),
	fDeferRegistration(true),
	fPendingCount(0),
	fActiveDevice(0),
	fCardType(CARD_TYPE_UNKNOWN)
{
	CALLED();

	// Get the parent info, it includes the API to send commands to the hardware
	device_node* parent = gDeviceManager->get_parent_node(node);
	fStatus = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fController, &fCookie);
	gDeviceManager->put_node(parent);

	if (fStatus != B_OK) {
		ERROR("Not able to establish the bus %s\n",
			strerror(fStatus));
		return;
	}

	fScanSemaphore = create_sem(0, "MMC bus scan");
	fLockSemaphore = create_sem(1, "MMC bus lock");
	fInitialScanDone = create_sem(0, "MMC bus initial scan");
	mutex_init(&fRegistrationLock, "MMC bus registration");

	// Give the controller the scan semaphore before the scanning thread
	// exists: the controller releases it right away if a device is present,
	// so its count now says whether the first scan will find something to
	// wait for (see RegisterInitialDevices()). With the thread already
	// running, it could have taken the semaphore in between.
	fController->set_scan_semaphore(fCookie, fScanSemaphore);
	int32 count = 0;
	if (get_sem_count(fScanSemaphore, &count) == B_OK && count > 0)
		fInitialScanRequested = true;

	fWorkerThread = spawn_kernel_thread(_WorkerThread, "SD bus controller",
		B_NORMAL_PRIORITY, this);
	resume_thread(fWorkerThread);
}


MMCBus::~MMCBus()
{
	CALLED();

	// Tell the worker thread we want to stop
	fStatus = B_SHUTTING_DOWN;

	// Delete the semaphores (this will unlock the worker thread if it was
	// waiting on them)
	delete_sem(fScanSemaphore);
	delete_sem(fLockSemaphore);

	// Wait for the worker thread to terminate
	status_t result;
	if (fWorkerThread > 0)
		wait_for_thread(fWorkerThread, &result);

	delete_sem(fInitialScanDone);
	mutex_destroy(&fRegistrationLock);
}


/*!	Called from the bus node's register_child_devices hook, while the device
	manager is registering the bus. Waits for the first scan and registers
	what it found, so that a device holding the boot volume exists before the
	kernel looks for boot partitions. The scan runs in the worker thread,
	which cannot register nodes itself meanwhile: the device manager lock is
	held here, so it would block on it, and we would wait for it forever.
	Later scans (a card inserted afterwards) register their nodes directly.
*/
void
MMCBus::RegisterInitialDevices()
{
	if (fInitialScanRequested && !fInitialScanWaited) {
		fInitialScanWaited = true;
		status_t status = acquire_sem_etc(fInitialScanDone, 1,
			B_RELATIVE_TIMEOUT, 3000000);
		if (status != B_OK) {
			ERROR("first scan not finished after 3s, the device will be "
				"published when it is\n");
		}
	}

	card_info cards[kMaxPendingCards];
	int32 count;
	{
		MutexLocker locker(fRegistrationLock);
		fDeferRegistration = false;
		count = fPendingCount;
		memcpy(cards, fPendingCards, sizeof(card_info) * count);
		fPendingCount = 0;
	}

	for (int32 i = 0; i < count; i++)
		_RegisterCard(cards[i]);

	if (fInitialScanRequested) {
		TRACE("%" B_PRId32 " device(s) published while registering the "
			"bus\n", count);
	}
}


void
MMCBus::_PublishCard(const card_info& card)
{
	{
		MutexLocker locker(fRegistrationLock);
		if (fDeferRegistration) {
			// The bus node is still being registered, RegisterInitialDevices()
			// will publish this
			if (fPendingCount < kMaxPendingCards)
				fPendingCards[fPendingCount++] = card;
			return;
		}
	}
	_RegisterCard(card);
}


void
MMCBus::_RegisterCard(const card_info& card)
{
	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc" }},
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "mmc device" }},
		{ B_DEVICE_VENDOR_ID, B_UINT32_TYPE, {.ui32 = card.vendor}},
		{ B_DEVICE_ID, B_STRING_TYPE, {.string = card.name}},
		{ B_DEVICE_UNIQUE_ID, B_UINT32_TYPE, {.ui32 = card.serial}},
		{ "mmc/revision", B_UINT16_TYPE, {.ui16 = card.revision}},
		{ "mmc/month", B_UINT8_TYPE, {.ui8 = card.month}},
		{ "mmc/year", B_UINT16_TYPE, {.ui16 = card.year}},
		{ kMmcRcaAttribute, B_UINT16_TYPE, {.ui16 = card.rca}},
		{ kMmcTypeAttribute, B_UINT8_TYPE, {.ui8 = card.type}},
		{}
	};

	if (card.type == CARD_TYPE_SDIO) {
		char name[48];
		snprintf(name, sizeof(name), "SDIO function %u (%04x:%04x)",
			card.function, card.sdioVendor, card.sdioDevice);
		device_attr sdioAttrs[] = {
			{ B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc" }},
			{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = name }},
			{ B_DEVICE_VENDOR_ID, B_UINT16_TYPE, {.ui16 = card.sdioVendor}},
			{ B_DEVICE_ID, B_UINT16_TYPE, {.ui16 = card.sdioDevice}},
			{ kMmcRcaAttribute, B_UINT16_TYPE, {.ui16 = card.rca}},
			{ kMmcTypeAttribute, B_UINT8_TYPE, {.ui8 = card.type}},
			{ kSdioFunctionAttribute, B_UINT8_TYPE, {.ui8 = card.function}},
			{ kSdioVendorAttribute, B_UINT16_TYPE, {.ui16 = card.sdioVendor}},
			{ kSdioDeviceAttribute, B_UINT16_TYPE, {.ui16 = card.sdioDevice}},
			{ kSdioClassAttribute, B_UINT8_TYPE, {.ui8 = card.sdioClass}},
			{}
		};
		gDeviceManager->register_node(fNode, MMC_BUS_MODULE_NAME, sdioAttrs,
			NULL, NULL);
		return;
	}

	// publish child device for the card
	gDeviceManager->register_node(fNode, MMC_BUS_MODULE_NAME, attrs, NULL,
		NULL);
}


void
MMCBus::_InitialScanDone()
{
	MutexLocker locker(fRegistrationLock);
	if (!fInitialScanSignaled) {
		fInitialScanSignaled = true;
		release_sem(fInitialScanDone);
	}
}


status_t
MMCBus::InitCheck()
{
	return fStatus;
}


void
MMCBus::Rescan()
{
	// Just wake up the thread for a scan
	release_sem(fScanSemaphore);
}


status_t
MMCBus::ExecuteCommand(uint16_t rca, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->execute_command(fCookie, command, argument, response);
}


status_t
MMCBus::DoIO(uint16_t rca, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->do_io(fCookie, command, operation, offsetAsSectors);
}


status_t
MMCBus::ReadData(uint16_t rca, uint8_t command, uint32_t argument,
	void* buffer, size_t length)
{
	if (fController->read_data == NULL)
		return B_NOT_SUPPORTED;

	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->read_data(fCookie, command, argument, buffer, length);
}


status_t
MMCBus::SetTiming(uint16_t rca, int timing, uint32_t flags)
{
	if (fController->set_timing == NULL)
		return B_NOT_SUPPORTED;

	// The controller checks that transfers still work after the change, so
	// the device must be selected
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->set_timing(fCookie, timing, flags);
}


void
MMCBus::SetClock(int frequency)
{
	fController->set_clock(fCookie, frequency);
}


void
MMCBus::SetBusWidth(int width)
{
	fController->set_bus_width(fCookie, width);
}


void
MMCBus::SetCardType(card_type type)
{
	fCardType = type;
	fController->set_card_type(fCookie, type);
}



/*!	Reads one byte of the register space of an SDIO function with CMD52
	(SDIO Simplified Specification 3.00, 5.1). Function 0 is the CCCR and the
	FBRs of the card.
*/
status_t
MMCBus::_ReadDirect(uint16_t rca, uint8_t function, uint32_t address,
	uint8_t* _value)
{
	// Used while the card is brought up: retry a few times, and say why a
	// read failed.
	status_t status = B_ERROR;
	for (int attempt = 0; attempt < 3; attempt++) {
		status = DirectIO(rca, false, function, address, 0, _value);
		if (status == B_OK)
			return B_OK;
		ERROR("SDIO CMD52 read of %u:%05" B_PRIx32 " failed (attempt %d): "
			"%s\n", function, address, attempt + 1, strerror(status));
		snooze(10000);
	}
	return status;
}


/*!	CMD52 (SDIO Simplified Specification 3.00, 5.1): reads or writes one
	byte of the register space of an SDIO function. Function 0 is the CCCR
	and the FBRs of the card. For a write, \a _result gets the value read
	back after it (the RAW flag).
*/
status_t
MMCBus::DirectIO(uint16_t rca, bool isWrite, uint8_t function,
	uint32_t address, uint8_t value, uint8_t* _result)
{
	if (function > 7 || address > 0x1FFFF)
		return B_BAD_VALUE;

	uint32_t argument = ((uint32_t)function << 28) | (address << 9);
	if (isWrite)
		argument |= (1u << 31) | (1 << 27) | value;

	uint32_t response;
	status_t status = ExecuteCommand(rca, SDIO_IO_RW_DIRECT, argument,
		&response);
	if (status != B_OK)
		return status;

	// R5: response flags in bits 15:8 (5.2). COM_CRC_ERROR, ILLEGAL_COMMAND,
	// ERROR, FUNCTION_NUMBER and OUT_OF_RANGE mean the access failed.
	if ((response & 0xCB00) != 0) {
		ERROR("CMD52 %s of %u:%05" B_PRIx32 ": R5 flags %02" B_PRIx32 "\n",
			isWrite ? "write" : "read", function, address,
			(response >> 8) & 0xFF);
		return B_IO_ERROR;
	}

	if (_result != NULL)
		*_result = response & 0xFF;
	return B_OK;
}


/*!	CMD53 (SDIO Simplified Specification 3.00, 5.3). \a blockCount 0 is
	byte mode with \a blockSize bytes, otherwise block mode with the block
	size the function driver has set in the FBR.
*/
status_t
MMCBus::ExtendedIO(uint16_t rca, bool isWrite, uint8_t function,
	uint32_t address, bool incrementAddress, void* buffer,
	uint32_t blockCount, uint32_t blockSize)
{
	if (fController->transfer_io == NULL)
		return B_NOT_SUPPORTED;
	if (function > 7 || address > 0x1FFFF || blockSize == 0
		|| blockCount > 511 || (blockCount == 0 && blockSize > 512)) {
		return B_BAD_VALUE;
	}

	uint32_t argument = ((uint32_t)function << 28) | (address << 9);
	if (isWrite)
		argument |= 1u << 31;
	if (incrementAddress)
		argument |= 1 << 26;
	if (blockCount > 0) {
		argument |= (1 << 27) | blockCount;
	} else {
		// byte mode: a count of 0 means 512 bytes
		argument |= blockSize & 0x1FF;
	}

	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;

	return fController->transfer_io(fCookie, argument, isWrite, buffer,
		blockCount > 0 ? blockCount : 1, blockSize);
}


status_t
MMCBus::SetCardInterrupt(void (*handler)(void*), void* cookie)
{
	if (fController->set_card_interrupt == NULL)
		return B_NOT_SUPPORTED;

	fController->set_card_interrupt(fCookie, handler, cookie);
	return B_OK;
}


/*!	Walks the CIS at \a cisPointer for the CISTPL_MANFID tuple (SDIO
	Simplified Specification 3.00, 16.7.1).
*/
status_t
MMCBus::_ReadCISManufacturer(uint16_t rca, uint32_t cisPointer,
	uint16_t* _vendor, uint16_t* _device)
{
	uint32_t address = cisPointer;
	for (int tuples = 0; tuples < 64; tuples++) {
		uint8_t code;
		uint8_t link;
		if (_ReadDirect(rca, 0, address, &code) != B_OK)
			return B_IO_ERROR;
		if (code == 0xFF)
			break;
			// CISTPL_END
		if (code == 0x00) {
			// CISTPL_NULL has no link field
			address++;
			continue;
		}
		if (_ReadDirect(rca, 0, address + 1, &link) != B_OK)
			return B_IO_ERROR;

		if (code == 0x20 && link >= 4) {
			// CISTPL_MANFID: TPLMID_MANF and TPLMID_CARD, little endian
			uint8_t bytes[4];
			for (int i = 0; i < 4; i++) {
				if (_ReadDirect(rca, 0, address + 2 + i, &bytes[i]) != B_OK)
					return B_IO_ERROR;
			}
			*_vendor = bytes[0] | (bytes[1] << 8);
			*_device = bytes[2] | (bytes[3] << 8);
			return B_OK;
		}
		if (link == 0xFF)
			break;
		address += 2 + link;
	}

	return B_ENTRY_NOT_FOUND;
}


/*!	Checks whether the card on the bus is an SDIO card, initializes it, and
	publishes one node for each of its I/O functions. Called after CMD0 and
	CMD8 (SDIO Simplified Specification 3.00, 3.1.2, figure 3-2). Returns
	\c false if the card does not answer CMD5, so that it is probed as a
	memory card.
*/
bool
MMCBus::_ProbeSDIO()
{
	uint32_t ocr = 0;
	status_t status = ExecuteCommand(0, SDIO_IO_SEND_OP_COND, 0, &ocr);
	if (status == B_BUSY) {
		// the command line is still busy after the CMD8 timeout
		snooze(10000);
		status = ExecuteCommand(0, SDIO_IO_SEND_OP_COND, 0, &ocr);
	}
	if (status != B_OK) {
		TRACE_ALWAYS("no answer to CMD5 (%s), not an SDIO card\n",
			strerror(status));
		return false;
	}

	uint8_t functions = (ocr >> 28) & 7;
	bool memoryPresent = (ocr & (1 << 27)) != 0;
	TRACE_ALWAYS("SDIO card: %u I/O functions%s, OCR %08" B_PRIx32 "\n",
		functions, memoryPresent ? ", with memory" : "", ocr);
	if (functions == 0)
		return false;

	// Ask for the voltages the card supports and wait for it to be ready.
	// 2.7 to 3.6V is all the SDHCI drivers set up.
	uint32_t voltages = ocr & 0x00FF8000;
	int attempt = 0;
	do {
		if (ExecuteCommand(0, SDIO_IO_SEND_OP_COND, voltages, &ocr) != B_OK) {
			ERROR("SDIO CMD5 with voltages %06" B_PRIx32 " failed\n",
				voltages);
			return true;
		}
		if ((ocr & (1u << 31)) != 0)
			break;
		snooze(10000);
	} while (++attempt < 100);
	if ((ocr & (1u << 31)) == 0) {
		ERROR("SDIO card did not get ready, OCR %08" B_PRIx32 "\n", ocr);
		return true;
	}

	SetCardType(CARD_TYPE_SDIO);

	uint32_t response;
	if (ExecuteCommand(0, SD_SEND_RELATIVE_ADDR, 0, &response) != B_OK) {
		ERROR("SDIO card did not answer CMD3\n");
		return true;
	}
	uint16_t rca = response >> 16;
	TRACE_ALWAYS("SDIO card: CMD3 answered %08" B_PRIx32 "\n", response);

	// CCCR (6.9): revisions, capabilities and the common CIS pointer
	uint8_t revision = 0;
	uint8_t specRevision = 0;
	uint8_t capability = 0;
	uint8_t cis[3] = {};
	if (_ReadDirect(rca, 0, 0x00, &revision) != B_OK
		|| _ReadDirect(rca, 0, 0x01, &specRevision) != B_OK
		|| _ReadDirect(rca, 0, 0x08, &capability) != B_OK
		|| _ReadDirect(rca, 0, 0x09, &cis[0]) != B_OK
		|| _ReadDirect(rca, 0, 0x0A, &cis[1]) != B_OK
		|| _ReadDirect(rca, 0, 0x0B, &cis[2]) != B_OK) {
		ERROR("SDIO card: reading the CCCR failed\n");
		return true;
	}
	uint32_t commonCis = cis[0] | (cis[1] << 8) | (cis[2] << 16);

	uint16_t vendor = 0;
	uint16_t device = 0;
	if (_ReadCISManufacturer(rca, commonCis, &vendor, &device) != B_OK)
		ERROR("SDIO card: no manufacturer in the CIS at %05" B_PRIx32 "\n",
			commonCis);

	TRACE_ALWAYS("SDIO card: RCA %04x, CCCR revision %02x, SD revision %02x, "
		"capabilities %02x, vendor %04x, device %04x\n", rca, revision,
		specRevision, capability, vendor, device);

	for (uint8_t function = 1; function <= functions; function++) {
		// FBR (6.10): the standard interface code is in the low nibble of
		// the first byte, 0xF means it is in the extended code byte
		uint8_t code = 0;
		_ReadDirect(rca, 0, function * 0x100, &code);
		uint8_t interfaceClass = code & 0x0F;
		if (interfaceClass == 0x0F)
			_ReadDirect(rca, 0, function * 0x100 + 1, &interfaceClass);

		TRACE_ALWAYS("SDIO function %u: class %02x\n", function,
			interfaceClass);

		card_info card = {};
		card.vendor = vendor;
		snprintf(card.name, sizeof(card.name), "%04x", device);
		card.rca = rca;
		card.type = CARD_TYPE_SDIO;
		card.function = function;
		card.sdioVendor = vendor;
		card.sdioDevice = device;
		card.sdioClass = interfaceClass;
		_PublishCard(card);
	}

	return true;
}

status_t
MMCBus::_ActivateDevice(uint16_t rca)
{
	// Do nothing if the device is already activated
	if (fActiveDevice == rca)
		return B_OK;

	uint32_t response;
	status_t result;
	result = fController->execute_command(fCookie, SELECT_DESELECT_CARD, ((uint32)rca) << 16,
		&response);

	if (result == B_OK)
		fActiveDevice = rca;

	return result;
}


void MMCBus::_AcquireScanSemaphore()
{
	ReleaseBus();
	acquire_sem(fScanSemaphore);
	AcquireBus();
}


void
MMCBus::_TerminateBus()
{
	fController->terminate_bus(fCookie);
}


status_t
MMCBus::_WorkerThread(void* cookie)
{
	MMCBus* bus = (MMCBus*)cookie;
	uint32_t response;

	bus->AcquireBus();

	// We assume the bus defaults to 400kHz clock and has already powered on
	// cards.

	// Reset all cards on the bus
	// This does not work if the bus has not been powered on yet (the command
	// will timeout), in that case we wait until asked to scan again when a
	// card has been inserted and powered on.
	status_t result;
	do {
		bus->_AcquireScanSemaphore();

		// Check if we need to exit early (possible if the parent device did
		// not manage initialize itself correctly)
		if (bus->fStatus == B_SHUTTING_DOWN) {
			bus->_InitialScanDone();
			bus->ReleaseBus();
			return B_OK;
		}

		// Start the identification clock before the first command. The
		// controller only sets it at startup when a card is already there:
		// for a card inserted later, the insertion interrupt powers the slot
		// on but leaves the clock alone (changing it there could corrupt a
		// command in flight), so CMD0 would go out with no clock and time out.
		// Setting it here, with the bus held, has no such race. The card
		// also needs 1ms after power on and 74 clock cycles before its first
		// command.
		bus->SetClock(400);
		snooze(1000);

		TRACE("Reset the bus...\n");
		result = bus->ExecuteCommand(0, GO_IDLE_STATE, 0, NULL);
		TRACE("CMD0 result: %s\n", strerror(result));

		// CMD0 has no response, so a device that did not get it (corrupted on
		// the wire) cannot tell us. Send it a second time: an eMMC device the
		// firmware left in the transfer state would otherwise stay there.
		if (result == B_OK) {
			snooze(1000);
			result = bus->ExecuteCommand(0, GO_IDLE_STATE, 0, NULL);
		}
	} while (result != B_OK);

	// Need to wait at least 8 clock cycles after CMD0 before sending the next
	// command. With the default 400kHz clock that would be 20 microseconds,
	// but we need to wait at least 20ms here, otherwise the next command times
	// out
	snooze(30000);

	while (bus->fStatus != B_SHUTTING_DOWN) {
		// An eMMC device is soldered to the board and never goes away. A
		// second scan request (for example from the card presence interrupt)
		// must not run the identification sequence again: the device has
		// left the idle state, so it rejects every probe command, and the
		// scan then wrongly reset the card type to SD. That in turn made the
		// SDHCI driver use SD response types for CMD6 and CMD8, and put the
		// bus back to 1-bit mode and 400kHz behind the disk driver's back.
		if (is_mmc_card_type(bus->fCardType)) {
			TRACE("rescan request ignored, eMMC already initialized\n");
			bus->_AcquireScanSemaphore();
			continue;
		}

		TRACE("Scanning the bus\n");

		// Use the low speed clock and 1bit bus width for scanning
		bus->SetClock(400);
		bus->SetBusWidth(1);

		// Probe the voltage range
		enum {
			// Table 4-40 in physical layer specification v8.00
			// All other values are currently reserved
			HOST_27_36V = 1, // Host supplied voltage 2.7-3.6V
		};

		// An arbitrary value, we just need to check that the response
		// containts the same.
		static const uint8 kVoltageCheckPattern = 0xAA;
		uint8_t cardType = CARD_TYPE_SD;
		// FIXME MMC cards will not reply to this! They expect CMD1 instead
		// SD v1 cards will also not reply, but we can proceed to ACMD41
		// If ACMD41 also does not work, it may be an SDIO card, too
		uint32_t probe = (HOST_27_36V << 8) | kVoltageCheckPattern;
		uint32_t hcs = 1 << 30;
		uint32_t ocr = 0;
		status_t status = bus->ExecuteCommand(0, SD_SEND_IF_COND, probe, &response);
		if (status == B_OK && response != probe) {
			// Not an SD 2.0 card echoing the check pattern. What answers here
			// instead is an eMMC device that missed the reset and is still in
			// the state the firmware left it in: for eMMC, CMD8 is SEND_EXT_CSD,
			// and the reply is its R1 card status. Reset it again, and go on
			// as for a device that does not implement CMD8 (the MMC path).
			// This used to terminate the bus, and the device never showed up.
			ERROR("CMD8 answered %08" B_PRIx32 " instead of the check pattern "
				"(an eMMC device not reset, card state %" B_PRIu32 "), "
				"resetting again\n", response, (response >> 9) & 0xF);
			bus->ExecuteCommand(0, GO_IDLE_STATE, 0, NULL);
			snooze(1000);
			bus->ExecuteCommand(0, GO_IDLE_STATE, 0, NULL);
			snooze(30000);
			status = B_ERROR;
		}
		// An SDIO card answers CMD5; memory cards and eMMC devices do not
		// (SDIO Simplified Specification 3.00, figure 3-2)
		if (bus->_ProbeSDIO()) {
			bus->SetClock(25000);
			bus->_InitialScanDone();
			bus->_AcquireScanSemaphore();
			continue;
		}

		if (status != B_OK) {
			TRACE("Card does not implement CMD8, may be a V1 SD or MMC card\n");
			// Do not check for SDHC support in this case
			hcs = 0;

			TRACE("Trying MMC CMD1 initialization...\n");
			do {
				// Full voltage window (2.7-3.6V and 1.70-1.95V), and bit 30 to
				// tell the device the host supports sector addressing, which
				// devices larger than 2GB require (JEDEC 84-B51, 7.1). Devices
				// that only do byte addressing ignore that bit.
				status = bus->ExecuteCommand(0, MMC_SEND_OP_COND, 0x40FF8080, &ocr);
				if (status != B_OK) {
					TRACE("MMC CMD1 failed\n");
					break;
				}
				if ((ocr & (1 << 31)) == 0) {
					TRACE("MMC card is busy\n");
					snooze(100000);
				}
			} while ((ocr & (1 << 31)) == 0);

			if (status == B_OK && (ocr & (1 << 31)) != 0) {
				TRACE("Detected MMC card after CMD1\n");
				TRACE("device ready after CMD1, OCR %08" B_PRIx32 "\n", ocr);
				if ((ocr & (1 << 30)) != 0)
					cardType = CARD_TYPE_MMC_EXTENDED_CAPACITY;
				else
					cardType = CARD_TYPE_MMC;
			}
		} else if (response != probe) {
			ERROR("Card does not support voltage range (expected %x, "
				"reply %x)\n", probe, response);
			bus->_TerminateBus();
			bus->_InitialScanDone();
			bus->ReleaseBus();
			return B_ERROR;
		}

		// Probe OCR, waiting for card to become ready
		// We keep repeating ACMD41 until the card replies that it is
		// initialized. For MMC we already probed using CMD1 above.
		if ((cardType != CARD_TYPE_MMC) && (cardType != CARD_TYPE_MMC_EXTENDED_CAPACITY)) {
			// A card that answers neither CMD1 nor ACMD41 (an SDIO card, or
			// a slot without a working card) used to keep this loop going
			// forever: ocr was never written, so whether it ended depended
			// on what was on the stack. An SD card has to finish its
			// initialization within 1 second (physical layer specification,
			// 4.2.3); give up after 2.
			bool ready = false;
			for (int attempt = 0; attempt < 20 && !ready; attempt++) {
				uint32_t cardStatus = 0;
				status = bus->ExecuteCommand(0, SD_APP_CMD, 0, &cardStatus);
				while (status == B_BUSY) {
					ERROR("Card locked after CMD8...\n");
					snooze(1000000);
					status = bus->ExecuteCommand(0, SD_APP_CMD, 0, &cardStatus);
				}
				if (status != B_OK) {
					TRACE("CMD55 failed: %s\n", strerror(status));
					break;
				}
				if ((cardStatus & 0xFFFF8000) != 0)
					ERROR("SD card reports error %x\n", cardStatus);
				if ((cardStatus & (1 << 5)) == 0)
					ERROR("Card did not enter ACMD mode\n");

				ocr = 0;
				status = bus->ExecuteCommand(0, SD_SEND_OP_COND,
					hcs | 0xFF8000, &ocr);
				if (status == B_OK && (ocr & (1u << 31)) != 0)
					ready = true;
				else {
					TRACE("Card is busy\n");
					snooze(100000);
				}
			}

			if (!ready) {
				ERROR("no card answered ACMD41, giving up on this scan\n");
				bus->_InitialScanDone();
				bus->_AcquireScanSemaphore();
				continue;
			}
		}

		// FIXME this should be asked to each card, when there are multiple
		// ones. So ACMD41 should be moved inside the probing loop below?
		if (cardType == CARD_TYPE_SD) {
			if ((ocr & hcs) != 0)
				cardType = CARD_TYPE_SDHC;
			if ((ocr & (1 << 29)) != 0)
				cardType = CARD_TYPE_UHS2;
			if ((ocr & (1 << 24)) != 0)
				TRACE("Card supports 1.8v");
		}
		TRACE("Voltage range: %x\n", ocr & 0xFFFFFF);

		// Set the card type so the next commands have the correct reply types
		// (MMC and SD commands with the same identifier sometime expect different responses)
		bus->SetCardType((card_type)cardType);

		// TODO send CMD11 to switch to low voltage mode if card supports it?

		// We use CMD2 (ALL_SEND_CID) and CMD3 (SEND_RELATIVE_ADDR) to assign
		// an RCA to all cards. Initially all cards have an RCA of 0 and will
		// all receive CMD2. But only one of them will reply (they do collision
		// detection while sending the CID in reply). We assign a new RCA to
		// that first card, and repeat the process with the remaining ones
		// until no one answers to CMD2. Then we know all cards have an RCA
		// (and a matching published device on our side).
		uint32_t cid[4];
		uint32_t vendor;
		char name[7];
		uint32_t serial;
		uint16_t revision;
		uint8_t month;
		uint16_t year;
		uint16_t rca;
		bool cardFound = false;
		// This being an if statement as opposed to a while statement restricts
		// it to one device per bus.
		if ((cardType == CARD_TYPE_MMC) || (cardType == CARD_TYPE_MMC_EXTENDED_CAPACITY)) {
			if (bus->ExecuteCommand(0, ALL_SEND_CID, 0, cid) == B_OK) {
				// We currently support only a single card, so use a fixed RCA.
				rca = 1;
				status
					= bus->ExecuteCommand(0, MMC_SET_RELATIVE_ADDR, ((uint32)rca) << 16, &response);
				TRACE("MMC RCA: %x Status: %x\n", rca, response & 0xFFFF);
				if (status != B_OK) {
					TRACE("Failed to set RCA for MMC card\n");
				} else {
					MMCCid mmcCid(cid);
					vendor = mmcCid.VendorID();
					mmcCid.ProductName(name);
					serial = mmcCid.ProductSerial();
					revision = mmcCid.ProductRevision();
					month = mmcCid.ManufactureMonth();
					year = mmcCid.ManufactureYear(true);
					TRACE("CID raw %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32
						" %08" B_PRIx32 "\n", mmcCid.Word(3), mmcCid.Word(2),
						mmcCid.Word(1), mmcCid.Word(0));
					TRACE_ALWAYS("CID: MID=0x%02" B_PRIx32 ", OID=0x%02x, name=\"%s\", "
						"PSN=%08" B_PRIx32 ", PRV=%u.%u, MDT=%u/%u, type=%s\n",
						vendor, mmcCid.OemID(), name, serial, revision / 100,
						revision % 100, month, year,
						cardType == CARD_TYPE_MMC_EXTENDED_CAPACITY
							? "high capacity (sector addressing)"
							: "standard capacity (byte addressing)");
					cardFound = true;
				}
			}
		} else if (bus->ExecuteCommand(0, ALL_SEND_CID, 0, cid) == B_OK) {
			bus->ExecuteCommand(0, SD_SEND_RELATIVE_ADDR, 0, &response);

			TRACE("RCA: %x Status: %x\n", response >> 16, response & 0xFFFF);

			if ((response & 0xFF00) != 0x500) {
				TRACE("Card did not enter data state\n");
				// This probably means there are no more cards to scan on the
				// bus, so exit the loop.
				break;
			}

			// The card now has an RCA and it entered the data phase, which
			// means our initializing job is over, we can pass it on to the
			// mmc_disk driver.
			rca = response >> 16;
			SDCid sdCid(cid);
			vendor = sdCid.VendorID();
			sdCid.ProductName(name);
			serial = sdCid.ProductSerial();
			revision = sdCid.ProductRevision();
			month = sdCid.ManufactureMonth();
			year = sdCid.ManufactureYear();
			cardFound = true;
		}

		if (cardFound) {
			card_info card;
			card.vendor = vendor;
			strlcpy(card.name, name, sizeof(card.name));
			card.serial = serial;
			card.revision = revision;
			card.month = month;
			card.year = year;
			card.rca = rca;
			card.type = cardType;
			bus->_PublishCard(card);
		}

		// TODO if there is a single card active, check if it supports CMD6
		// (spec version 1.10 or later in SCR). If it does, check if CMD6 can
		// enable high speed mode, use that to go to 50MHz instead of 25.
		bus->SetClock(25000);

		// The first scan is over, whether it found something or not
		bus->_InitialScanDone();

		// FIXME we also need to unpublish devices that are gone. Probably need
		// to "ping" all RCAs somehow? Or is there an interrupt we can look for
		// to detect added/removed cards?

		// Wait for the next scan request
		// The thread will spend most of its time waiting here
		bus->_AcquireScanSemaphore();
	}

	bus->_InitialScanDone();
	bus->ReleaseBus();

	TRACE("poller thread terminating");
	return B_OK;
}
