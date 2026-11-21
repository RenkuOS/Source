/*
 * Copyright 2018-2021 Intel Corporation. All rights reserved.
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the BSD 3-Clause license.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Boots Sound Open Firmware on the Bay Trail LPE audio DSP. The sequence
 * follows the Linux SOF driver (sound/soc/sof/intel/byt.c, atom.c and
 * sound/soc/sof/ipc3-loader.c), see sof_dsp.h.
 */


#include "sof_dsp.h"

#include <ACPI.h>
#include <KernelExport.h>

#include <stdio.h>
#include <string.h>

extern "C" {
#	include "acpi.h"
}


#define TRACE_ALWAYS(x...)	dprintf("intel_lpe: " x)

#define SOF_EXT_MAN_MAGIC		0x6e614d58	// "XMan"
#define SOF_FW_SIGNATURE		"Reef"

#define SOF_FW_BLK_TYPE_IRAM	1
#define SOF_FW_BLK_TYPE_DRAM	2
#define SOF_FW_BLK_TYPE_SRAM	3

#define DSP_RUN_TIMEOUT			1000000
#define DSP_BOOT_TIMEOUT		3000000


static uint32
read32(lpe_dsp* dsp, uint32 offset)
{
	return *(volatile uint32*)(dsp->registers + offset);
}


static void
write32(lpe_dsp* dsp, uint32 offset, uint32 value)
{
	*(volatile uint32*)(dsp->registers + offset) = value;
}


/*!	The shim registers are 64 bit wide; like Linux on 32 bit, this reads and
	writes them as two halves, the low one first.
*/
static uint64
shim_read64(lpe_dsp* dsp, uint32 reg)
{
	uint32 offset = LPE_SHIM_OFFSET + reg;
	uint64 low = read32(dsp, offset);
	uint64 high = read32(dsp, offset + 4);
	return low | (high << 32);
}


static void
shim_write64(lpe_dsp* dsp, uint32 reg, uint64 value)
{
	uint32 offset = LPE_SHIM_OFFSET + reg;
	write32(dsp, offset, (uint32)value);
	write32(dsp, offset + 4, (uint32)(value >> 32));
}


static void
shim_update64(lpe_dsp* dsp, uint32 reg, uint64 mask, uint64 value)
{
	uint64 old = shim_read64(dsp, reg);
	uint64 updated = (old & ~mask) | (value & mask);
	if (updated != old)
		shim_write64(dsp, reg, updated);
}


/*!	\a offset is the offset in the BAR, as the firmware reports them. */
static void
mailbox_read(lpe_dsp* dsp, uint32 offset, void* buffer, size_t size)
{
	if (offset > LPE_BAR_SIZE || size > LPE_BAR_SIZE - offset) {
		memset(buffer, 0, size);
		return;
	}

	uint32* words = (uint32*)buffer;
	for (size_t i = 0; i < size / 4; i++)
		words[i] = read32(dsp, offset + i * 4);
}


static uint32
get32(const uint8* data)
{
	uint32 value;
	memcpy(&value, data, sizeof(value));
	return value;
}


static void
log_state(lpe_dsp* dsp, const char* when)
{
	TRACE_ALWAYS("DSP %s: CSR %#" B_PRIx64 ", IPCX %#" B_PRIx64 ", IPCD %#"
		B_PRIx64 ", IMRX %#" B_PRIx64 ", IMRD %#" B_PRIx64 "\n", when,
		shim_read64(dsp, SHIM_CSR), shim_read64(dsp, SHIM_IPCX),
		shim_read64(dsp, SHIM_IPCD), shim_read64(dsp, SHIM_IMRX),
		shim_read64(dsp, SHIM_IMRD));
}


//	#pragma mark - ACPI


static acpi_status
parse_lpe_resources(acpi_resource* resource, void* context)
{
	lpe_dsp* dsp = (lpe_dsp*)context;

	phys_addr_t base = 0;
	size_t length = 0;
	switch (resource->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			base = resource->Data.FixedMemory32.Address;
			length = resource->Data.FixedMemory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_MEMORY32:
			base = resource->Data.Memory32.Minimum;
			length = resource->Data.Memory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_IRQ:
			if (dsp->irq == 0)
				dsp->irq = resource->Data.Irq.Interrupts[0];
			return AE_OK;
		case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
			if (dsp->irq == 0)
				dsp->irq = resource->Data.ExtendedIrq.Interrupts[0];
			return AE_OK;
		default:
			return AE_OK;
	}

	// in order: the DSP (LPE0), the PCI configuration shadow (LPE1) and
	// the IMR (LPE2), which this driver does not use
	if (dsp->physicalBase == 0) {
		dsp->physicalBase = base;
		dsp->length = length;
	} else if (dsp->pciBase == 0)
		dsp->pciBase = base;
	return AE_OK;
}


/*!	Finds the LPE in ACPI, switches it on and maps its registers. */
status_t
lpe_dsp_init(lpe_dsp* dsp)
{
	memset(dsp, 0, sizeof(*dsp));
	dsp->area = -1;
	dsp->pciArea = -1;

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return B_ERROR;

	char path[255];
	acpi_handle handle;
	status_t status = acpi->get_device("80860F28", 0, path, sizeof(path));
	if (status == B_OK)
		status = acpi->get_handle(NULL, path, &handle);
	if (status != B_OK) {
		TRACE_ALWAYS("no LPE audio DSP (80860F28) in ACPI\n");
		put_module(B_ACPI_MODULE_NAME);
		return B_DEVICE_NOT_FOUND;
	}

	status = acpi->walk_resources(handle, (char*)"_CRS", parse_lpe_resources,
		dsp);
	if (status != B_OK || dsp->physicalBase == 0
		|| dsp->length < LPE_BAR_SIZE) {
		TRACE_ALWAYS("LPE at %s has no usable memory resource\n", path);
		put_module(B_ACPI_MODULE_NAME);
		return B_DEVICE_NOT_FOUND;
	}
	TRACE_ALWAYS("LPE at %s: DSP %#" B_PRIxPHYSADDR " (%#" B_PRIxSIZE
		"), PCI shadow %#" B_PRIxPHYSADDR ", IRQ %" B_PRIu32 "\n", path,
		dsp->physicalBase, dsp->length, dsp->pciBase, dsp->irq);

	if (dsp->pciBase != 0) {
		dsp->pciArea = map_physical_memory("intel_lpe PCI shadow",
			dsp->pciBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&dsp->pci);
		if (dsp->pciArea >= 0) {
			TRACE_ALWAYS("LPE power state before _ON: %#" B_PRIx32 "\n",
				*(volatile uint32*)(dsp->pci + LPE_PCI_PMCSR));
		}
	}

	// the power resource of the LPE (_PR0) switches it to D0
	char method[300];
	snprintf(method, sizeof(method), "%s.PLPE._ON", path);
	status = acpi->evaluate_method(NULL, method, NULL, NULL);
	if (status != B_OK)
		TRACE_ALWAYS("%s failed: %s\n", method, strerror(status));
	put_module(B_ACPI_MODULE_NAME);
	snooze(1000);

	if (dsp->pciArea >= 0) {
		TRACE_ALWAYS("LPE power state after _ON: %#" B_PRIx32 "\n",
			*(volatile uint32*)(dsp->pci + LPE_PCI_PMCSR));
	}

	dsp->area = map_physical_memory("intel_lpe DSP", dsp->physicalBase,
		LPE_BAR_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&dsp->registers);
	if (dsp->area < 0) {
		TRACE_ALWAYS("mapping the DSP failed\n");
		lpe_dsp_uninit(dsp);
		return dsp->area;
	}

	log_state(dsp, "found");
	return B_OK;
}


void
lpe_dsp_uninit(lpe_dsp* dsp)
{
	if (dsp->area >= 0) {
		// put the DSP back into reset, with its interrupts masked
		shim_update64(dsp, SHIM_IMRX, 0x3, 0x3);
		shim_update64(dsp, SHIM_IMRD, 0x3, 0x3);
		shim_update64(dsp, SHIM_CSR,
			SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL,
			SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL);
		delete_area(dsp->area);
		dsp->area = -1;
	}
	if (dsp->pciArea >= 0) {
		delete_area(dsp->pciArea);
		dsp->pciArea = -1;
	}
}


//	#pragma mark - firmware


#define SOF_EXT_MAN_ELEM_WINDOW	1


/*!	Takes the mailbox windows from the window element of the extended
	manifest (struct sof_ext_man_window): an element header, an IPC
	extended data header, the number of windows and the windows.
*/
static void
parse_windows(lpe_dsp* dsp, const uint8* element, uint32 size)
{
	if (size < 8 + 16)
		return;
	uint32 count = get32(element + 8 + 12);
	for (uint32 w = 0; w < count && 8 + 16 + (w + 1) * 24 <= size; w++) {
		// struct sof_ipc_window_elem: size, type, id, flags, size, offset
		const uint8* window = element + 8 + 16 + w * 24;
		uint32 type = get32(window + 4);
		uint32 windowSize = get32(window + 16);
		uint32 relativeOffset = get32(window + 20);
		// the image may be damaged: every window must lie in the BAR
		if (relativeOffset > LPE_BAR_SIZE - LPE_MBOX_OFFSET
			|| windowSize > LPE_BAR_SIZE - LPE_MBOX_OFFSET - relativeOffset) {
			TRACE_ALWAYS("window type %" B_PRIu32 " at %#" B_PRIx32 ", %#"
				B_PRIx32 " bytes is outside the BAR, ignored\n", type,
				relativeOffset, windowSize);
			continue;
		}
		uint32 windowOffset = LPE_MBOX_OFFSET + relativeOffset;
		TRACE_ALWAYS("window type %" B_PRIu32 ": %#" B_PRIx32 ", %#" B_PRIx32
			" bytes\n", type, windowOffset, windowSize);
		switch (type) {
			case SOF_IPC_REGION_DOWNBOX:
				dsp->hostBoxOffset = windowOffset;
				dsp->hostBoxSize = windowSize;
				break;
			case SOF_IPC_REGION_UPBOX:
				dsp->dspBoxOffset = windowOffset;
				dsp->dspBoxSize = windowSize;
				break;
			case SOF_IPC_REGION_STREAM:
				dsp->streamBoxOffset = windowOffset;
				break;
			case SOF_IPC_REGION_EXCEPTION:
				dsp->exceptionOffset = windowOffset;
				break;
		}
	}
}


/*!	Copies the blocks of a SOF image ("Reef" header, optionally after an
	extended manifest) into the DSP memories.
*/
static status_t
load_image(lpe_dsp* dsp, const uint8* image, size_t size)
{
	size_t offset = 0;
	if (size >= 16 && get32(image) == SOF_EXT_MAN_MAGIC) {
		// extended manifest: magic, full size, header size, version, then
		// elements of a type and a size
		offset = get32(image + 4);
		uint32 position = get32(image + 8);
		if (offset >= size || position > offset)
			return B_BAD_DATA;
		while (offset - position >= 8) {
			uint32 type = get32(image + position);
			uint32 elementSize = get32(image + position + 4);
			if (elementSize < 8 || elementSize > offset - position)
				break;
			if (type == SOF_EXT_MAN_ELEM_WINDOW)
				parse_windows(dsp, image + position, elementSize);
			position += elementSize;
		}
	}

	// header: signature, file size, number of modules, ABI
	if (size - offset < 16
		|| memcmp(image + offset, SOF_FW_SIGNATURE, 4) != 0) {
		TRACE_ALWAYS("firmware: no \"Reef\" header\n");
		return B_BAD_DATA;
	}
	uint32 moduleCount = get32(image + offset + 8);
	const uint8* position = image + offset + 16;
	const uint8* end = image + size;

	uint32 blockCount = 0;
	uint32 byteCount = 0;
	for (uint32 m = 0; m < moduleCount; m++) {
		// module: type, size (without this header), number of blocks
		if (end - position < 12)
			return B_BAD_DATA;
		uint32 moduleSize = get32(position + 4);
		uint32 blocks = get32(position + 8);
		const uint8* block = position + 12;
		if ((size_t)(end - block) < moduleSize)
			return B_BAD_DATA;
		const uint8* moduleEnd = block + moduleSize;

		for (uint32 b = 0; b < blocks; b++) {
			// block: type, size (without this header), offset in the BAR
			if (moduleEnd - block < 12)
				return B_BAD_DATA;
			uint32 type = get32(block);
			uint32 blockSize = get32(block + 4);
			uint32 target = get32(block + 8);
			const uint8* data = block + 12;
			if ((size_t)(moduleEnd - data) < blockSize)
				return B_BAD_DATA;
			block = data + blockSize;

			if (blockSize == 0)
				continue;
			if (type != SOF_FW_BLK_TYPE_IRAM && type != SOF_FW_BLK_TYPE_DRAM
				&& type != SOF_FW_BLK_TYPE_SRAM) {
				continue;
			}
			if ((blockSize % 4) != 0 || target > LPE_BAR_SIZE
				|| blockSize > LPE_BAR_SIZE - target) {
				TRACE_ALWAYS("firmware: bad block %" B_PRIu32 " (type %"
					B_PRIu32 ", %#" B_PRIx32 " bytes at %#" B_PRIx32 ")\n", b,
					type, blockSize, target);
				return B_BAD_DATA;
			}

			for (uint32 i = 0; i < blockSize; i += 4)
				write32(dsp, target + i, get32(data + i));
			blockCount++;
			byteCount += blockSize;
		}
		position = moduleEnd;
	}

	TRACE_ALWAYS("firmware: %" B_PRIu32 " modules, %" B_PRIu32 " blocks, %"
		B_PRIu32 " bytes copied\n", moduleCount, blockCount, byteCount);
	return blockCount > 0 ? B_OK : B_BAD_DATA;
}


/*!	Reads the FW_READY message, see struct sof_ipc_fw_ready. */
static void
handle_fw_ready(lpe_dsp* dsp)
{
	uint8 message[96];
	mailbox_read(dsp, LPE_MBOX_OFFSET, message, sizeof(message));


	// struct sof_ipc_fw_version at 24: header (4), major, minor, micro,
	// build (2 each), date[12], time[10], tag[6], abi_version
	const uint8* version = message + 24;
	uint16 numbers[4];
	memcpy(numbers, version + 4, sizeof(numbers));
	char date[13], time[11], tag[7];
	memcpy(date, version + 12, 12);
	date[12] = '\0';
	memcpy(time, version + 24, 10);
	time[10] = '\0';
	memcpy(tag, version + 34, 6);
	tag[6] = '\0';
	uint32 abi = get32(version + 40);

	TRACE_ALWAYS("firmware ready: SOF %u.%u.%u build %u, tag %s, %s %s, ABI "
		"%" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32 "\n", numbers[0], numbers[1],
		numbers[2], numbers[3], tag, date, time, (abi >> 24) & 0xff,
		(abi >> 12) & 0xfff, abi & 0xfff);

	if (dsp->hostBoxSize == 0 || dsp->dspBoxSize == 0)
		TRACE_ALWAYS("the firmware image described no mailbox windows\n");
	dsp->ready = true;
}


/*!	Handles a message the DSP sent, if there is one, and tells the DSP that
	the host is done with it.
*/
static bool
handle_dsp_message(lpe_dsp* dsp)
{
	uint64 ipcd = shim_read64(dsp, SHIM_IPCD);
	if ((ipcd & SHIM_BYT_IPC_BUSY) == 0)
		return false;

	if (((uint32)ipcd & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
		TRACE_ALWAYS("DSP panic: code %#" B_PRIx32 ", IPCD %#" B_PRIx64 "\n",
			(uint32)ipcd, ipcd);
	} else {
		// FW_READY comes in the start of the mailbox, later messages in
		// the DSP box it announced
		uint32 header[2];
		mailbox_read(dsp, dsp->ready ? dsp->dspBoxOffset : LPE_MBOX_OFFSET,
			header, sizeof(header));
		if (!dsp->ready && (header[1] & SOF_GLB_TYPE_MASK) == SOF_IPC_FW_READY)
			handle_fw_ready(dsp);
		else if ((header[1] & SOF_GLB_TYPE_MASK) == SOF_IPC_GLB_TRACE_MSG) {
			// trace positions: nothing to do without a trace buffer
		} else {
			TRACE_ALWAYS("DSP message %#" B_PRIx32 " (%" B_PRIu32 " bytes)\n",
				header[1], header[0]);
		}
	}

	// clear BUSY and set DONE: the host accepts new messages
	shim_update64(dsp, SHIM_IPCD, SHIM_BYT_IPC_BUSY | SHIM_BYT_IPC_DONE,
		SHIM_BYT_IPC_DONE);
	return true;
}


status_t
lpe_dsp_boot(lpe_dsp* dsp, const uint8* image, size_t size)
{
	// The doorbells are polled for now: keep both interrupt directions
	// masked.
	shim_update64(dsp, SHIM_IMRX, 0x3, 0x3);
	shim_update64(dsp, SHIM_IMRD, 0x3, 0x3);

	// reset with the reset vector selected and the core stalled, then out
	// of reset, still stalled, for loading
	shim_update64(dsp, SHIM_CSR,
		SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL | SHIM_BYT_CSR_STALL,
		SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL | SHIM_BYT_CSR_STALL);
	spin(15);
	shim_update64(dsp, SHIM_CSR, SHIM_BYT_CSR_RST, 0);
	log_state(dsp, "reset");

	status_t status = load_image(dsp, image, size);
	if (status != B_OK) {
		TRACE_ALWAYS("firmware image not usable: %s\n", strerror(status));
		return status;
	}

	// release the stall and wait until the core runs
	shim_update64(dsp, SHIM_CSR, SHIM_BYT_CSR_STALL, 0);
	bigtime_t timeout = system_time() + DSP_RUN_TIMEOUT;
	while ((shim_read64(dsp, SHIM_CSR) & SHIM_BYT_CSR_PWAITMODE) != 0) {
		if (system_time() > timeout) {
			log_state(dsp, "did not start");
			return B_TIMED_OUT;
		}
		snooze(10000);
	}

	// the firmware announces itself with FW_READY
	timeout = system_time() + DSP_BOOT_TIMEOUT;
	while (!dsp->ready) {
		if (!handle_dsp_message(dsp) && system_time() > timeout) {
			log_state(dsp, "boot timed out");
			uint32 words[12];
			mailbox_read(dsp, LPE_MBOX_OFFSET, words, sizeof(words));
			for (int i = 0; i < 12; i += 4) {
				TRACE_ALWAYS("mailbox %#04x: %08" B_PRIx32 " %08" B_PRIx32
					" %08" B_PRIx32 " %08" B_PRIx32 "\n", i * 4, words[i],
					words[i + 1], words[i + 2], words[i + 3]);
			}
			return B_TIMED_OUT;
		}
		snooze(1000);
	}

	log_state(dsp, "running");
	return B_OK;
}


void
lpe_dsp_poll_messages(lpe_dsp* dsp)
{
	if (dsp->area >= 0 && dsp->ready)
		handle_dsp_message(dsp);
}
