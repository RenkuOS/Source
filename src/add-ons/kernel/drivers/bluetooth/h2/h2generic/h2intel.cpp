/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


// Firmware loader for Intel Bluetooth controllers with the secure boot
// loader (hardware variants 0x0b and 0x0c). After power-on, these
// controllers run a boot loader that accepts only a few vendor commands.
// The host sends the operational firmware from an .sfi file, boots it,
// and then sends the device configuration from a .ddc file.


#include "h2intel.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ByteOrder.h>
#include <FindDirectory.h>
#include <KernelExport.h>
#include <StorageDefs.h>

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <lock.h>

#include "h2debug.h"


#define INTEL_OP_RESET				0xfc01
#define INTEL_OP_READ_VERSION		0xfc05
#define INTEL_OP_SECURE_SEND		0xfc09
#define INTEL_OP_READ_BOOT_PARAMS	0xfc0d
#define INTEL_OP_WRITE_DDC			0xfc8b

#define INTEL_EVENT_VENDOR			0xff
#define INTEL_VENDOR_BOOTUP			0x02
#define INTEL_VENDOR_SECURE_SEND	0x06

#define INTEL_HW_PLATFORM			0x37
#define INTEL_FW_BOOTLOADER			0x06
#define INTEL_FW_OPERATIONAL		0x23

#define INTEL_FRAGMENT_INIT			0x00
#define INTEL_FRAGMENT_DATA			0x01
#define INTEL_FRAGMENT_SIGNATURE	0x02
#define INTEL_FRAGMENT_PUBLIC_KEY	0x03

// The .sfi file starts with a 128 byte CSS header, a 256 byte public key,
// 4 bytes of padding and a 256 byte signature. HCI commands follow.
#define SFI_CSS_HEADER_LENGTH		128
#define SFI_PUBLIC_KEY_OFFSET		128
#define SFI_PUBLIC_KEY_LENGTH		256
#define SFI_SIGNATURE_OFFSET		388
#define SFI_SIGNATURE_LENGTH		256
#define SFI_HEADER_LENGTH			644

#define MAX_FIRMWARE_SIZE			(2 * 1024 * 1024)
#define MAX_SECURE_SEND_FRAGMENT	252
#define EVENT_QUEUE_LENGTH			16
#define MAX_RX_ERRORS				10

static const bigtime_t kCommandTimeout = 2000000;
static const bigtime_t kDownloadTimeout = 5000000;
static const bigtime_t kBootTimeout = 5000000;


struct intel_version {
	uint8	status;
	uint8	hw_platform;
	uint8	hw_variant;
	uint8	hw_revision;
	uint8	fw_variant;
	uint8	fw_revision;
	uint8	fw_build_num;
	uint8	fw_build_ww;
	uint8	fw_build_yy;
	uint8	fw_patch_num;
} _PACKED;

struct intel_boot_params {
	uint8	status;
	uint8	otp_format;
	uint8	otp_content;
	uint8	otp_patch;
	uint16	dev_revid;
	uint8	secure_boot;
	uint8	key_from_hdr;
	uint8	key_type;
	uint8	otp_lock;
	uint8	api_lock;
	uint8	debug_lock;
	uint8	otp_bdaddr[6];
	uint8	min_fw_build_nn;
	uint8	min_fw_build_cw;
	uint8	min_fw_build_yy;
	uint8	limited_cce;
	uint8	unlocked_state;
} _PACKED;

struct intel_reset {
	uint8	reset_type;
	uint8	patch_enable;
	uint8	ddc_reload;
	uint8	boot_option;
	uint32	boot_param;
} _PACKED;


struct intel_loader;

struct intel_rx {
	intel_loader*	loader;
	usb_pipe		pipe;
	bool			bulk;
	uint8*			buffer;
	size_t			size;
	int32			errors;
};

struct intel_loader {
	bt_usb_dev*		bdev;

	mutex			lock;
	sem_id			eventSem;
	uint8			events[EVENT_QUEUE_LENGTH][HCI_MAX_EVENT_SIZE];
	size_t			eventLength[EVENT_QUEUE_LENGTH];
	uint32			eventHead;
	uint32			eventCount;

	intel_rx		rx[2];
	int32			pendingRx;
	bool			stopping;

	sem_id			bulkOutSem;
	status_t		bulkOutStatus;
	uint8			command[3 + 255];

	// Set from vendor events
	int32			downloadResult;
	bool			booted;
};


static const uint16 kIntelDevices[] = {
	0x0a2b,		// Wireless 8260 and 8265
};


static void rx_complete(void* cookie, status_t status, void* data,
	size_t actualLength);


static status_t
queue_rx(intel_rx* rx)
{
	if (rx->bulk) {
		return usb->queue_bulk(rx->pipe, rx->buffer, rx->size, rx_complete,
			rx);
	}

	return usb->queue_interrupt(rx->pipe, rx->buffer, rx->size, rx_complete,
		rx);
}


// Events arrive on the interrupt endpoint. In boot loader mode, the
// controller also sends events on the bulk endpoint.
static void
rx_complete(void* cookie, status_t status, void* data, size_t actualLength)
{
	intel_rx* rx = (intel_rx*)cookie;
	intel_loader* loader = rx->loader;
	uint8* event = (uint8*)data;

	if (status == B_OK && actualLength >= 2
		&& actualLength >= 2 + (size_t)event[1]
		&& actualLength <= HCI_MAX_EVENT_SIZE) {
		rx->errors = 0;

		mutex_lock(&loader->lock);
		if (loader->eventCount < EVENT_QUEUE_LENGTH) {
			uint32 index = (loader->eventHead + loader->eventCount)
				% EVENT_QUEUE_LENGTH;
			memcpy(loader->events[index], event, actualLength);
			loader->eventLength[index] = actualLength;
			loader->eventCount++;
		} else
			ERROR("intel: event queue full, event 0x%02x lost\n", event[0]);
		mutex_unlock(&loader->lock);

		release_sem_etc(loader->eventSem, 1, B_DO_NOT_RESCHEDULE);
	} else if (status != B_OK && status != B_CANCELED)
		rx->errors++;

	if (status != B_CANCELED && !loader->stopping
		&& rx->errors < MAX_RX_ERRORS && queue_rx(rx) == B_OK) {
		return;
	}

	atomic_add(&loader->pendingRx, -1);
}


static status_t
start_rx(intel_loader* loader)
{
	bt_usb_dev* bdev = loader->bdev;

	loader->rx[0].pipe = bdev->intr_in_ep->handle;
	loader->rx[0].bulk = false;
	loader->rx[0].size = max_c(HCI_MAX_EVENT_SIZE,
		bdev->max_packet_size_intr_in);
	loader->rx[1].pipe = bdev->bulk_in_ep->handle;
	loader->rx[1].bulk = true;
	loader->rx[1].size = max_c(HCI_MAX_FRAME_SIZE,
		bdev->max_packet_size_bulk_in);

	for (int i = 0; i < 2; i++) {
		intel_rx* rx = &loader->rx[i];
		rx->loader = loader;
		rx->errors = 0;
		rx->buffer = (uint8*)malloc(rx->size);
		if (rx->buffer == NULL)
			return B_NO_MEMORY;

		atomic_add(&loader->pendingRx, 1);
		status_t status = queue_rx(rx);
		if (status != B_OK) {
			atomic_add(&loader->pendingRx, -1);
			return status;
		}
	}

	return B_OK;
}


static void
stop_rx(intel_loader* loader)
{
	loader->stopping = true;
	usb->cancel_queued_transfers(loader->rx[0].pipe);
	usb->cancel_queued_transfers(loader->rx[1].pipe);

	for (int i = 0; i < 100 && atomic_get(&loader->pendingRx) > 0; i++)
		snooze(10000);

	if (atomic_get(&loader->pendingRx) > 0) {
		ERROR("intel: receive transfers did not stop\n");
		return;
	}

	free(loader->rx[0].buffer);
	free(loader->rx[1].buffer);
	loader->rx[0].buffer = NULL;
	loader->rx[1].buffer = NULL;
}


static status_t
next_event(intel_loader* loader, uint8* event, size_t* length,
	bigtime_t deadline)
{
	while (true) {
		mutex_lock(&loader->lock);
		if (loader->eventCount > 0) {
			uint32 index = loader->eventHead;
			*length = loader->eventLength[index];
			memcpy(event, loader->events[index], *length);
			loader->eventHead = (index + 1) % EVENT_QUEUE_LENGTH;
			loader->eventCount--;
			mutex_unlock(&loader->lock);
		} else {
			mutex_unlock(&loader->lock);

			status_t status = acquire_sem_etc(loader->eventSem, 1,
				B_ABSOLUTE_TIMEOUT, deadline);
			if (status != B_OK)
				return status;
			continue;
		}

		// Vendor events can arrive while a command waits for its
		// completion.
		if (*length >= 4 && event[0] == INTEL_EVENT_VENDOR) {
			if (event[2] == INTEL_VENDOR_SECURE_SEND)
				loader->downloadResult = event[3];
			else if (event[2] == INTEL_VENDOR_BOOTUP)
				loader->booted = true;
		}

		return B_OK;
	}
}


static void
bulk_out_complete(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	intel_loader* loader = (intel_loader*)cookie;
	loader->bulkOutStatus = status;
	release_sem(loader->bulkOutSem);
}


static status_t
send_command(intel_loader* loader, uint16 opcode, const void* params,
	uint8 length, bool bulk)
{
	bt_usb_dev* bdev = loader->bdev;

	loader->command[0] = opcode & 0xff;
	loader->command[1] = opcode >> 8;
	loader->command[2] = length;
	if (length > 0)
		memcpy(loader->command + 3, params, length);

	if (!bulk) {
		size_t actualLength;
		return usb->send_request(bdev->dev, bdev->ctrl_req, 0, 0, 0,
			3 + length, loader->command, &actualLength);
	}

	status_t status = usb->queue_bulk(bdev->bulk_out_ep->handle,
		loader->command, 3 + length, bulk_out_complete, loader);
	if (status != B_OK)
		return status;

	status = acquire_sem_etc(loader->bulkOutSem, 1, B_RELATIVE_TIMEOUT,
		kCommandTimeout);
	if (status != B_OK) {
		usb->cancel_queued_transfers(bdev->bulk_out_ep->handle);
		acquire_sem(loader->bulkOutSem);
		return status;
	}

	return loader->bulkOutStatus;
}


// Sends a command and waits for its Command Complete event. The first
// return parameter of all these commands is a status.
static status_t
intel_command(intel_loader* loader, uint16 opcode, const void* params,
	uint8 length, bool bulk, void* reply = NULL, size_t replySize = 0,
	size_t* replyLength = NULL)
{
	status_t status = send_command(loader, opcode, params, length, bulk);
	if (status != B_OK) {
		ERROR("intel: cannot send command 0x%04x: %s\n", opcode,
			strerror(status));
		return status;
	}

	bigtime_t deadline = system_time() + kCommandTimeout;
	uint8 event[HCI_MAX_EVENT_SIZE];
	size_t eventLength;

	while (true) {
		status = next_event(loader, event, &eventLength, deadline);
		if (status != B_OK) {
			ERROR("intel: no reply to command 0x%04x: %s\n", opcode,
				strerror(status));
			return status;
		}

		if (event[0] == HCI_EVENT_CMD_COMPLETE && eventLength >= 6
			&& (event[3] | (event[4] << 8)) == opcode) {
			if (event[5] != 0) {
				ERROR("intel: command 0x%04x failed, status 0x%02x\n",
					opcode, event[5]);
				return B_IO_ERROR;
			}

			size_t copyLength = min_c(eventLength - 5, replySize);
			if (reply != NULL)
				memcpy(reply, event + 5, copyLength);
			if (replyLength != NULL)
				*replyLength = eventLength - 5;
			return B_OK;
		}

		if (event[0] == HCI_EVENT_CMD_STATUS && eventLength >= 6
			&& (event[4] | (event[5] << 8)) == opcode && event[2] != 0) {
			ERROR("intel: command 0x%04x rejected, status 0x%02x\n",
				opcode, event[2]);
			return B_IO_ERROR;
		}
	}
}


static status_t
read_version(intel_loader* loader, intel_version* version)
{
	size_t length;
	status_t status = intel_command(loader, INTEL_OP_READ_VERSION, NULL, 0,
		false, version, sizeof(*version), &length);
	if (status != B_OK)
		return status;

	if (length != sizeof(*version)) {
		ERROR("intel: version reply has %" B_PRIuSIZE " bytes\n", length);
		return B_BAD_DATA;
	}

	return B_OK;
}


static status_t
secure_send(intel_loader* loader, uint8 type, const uint8* data,
	size_t length)
{
	uint8 params[1 + MAX_SECURE_SEND_FRAGMENT];

	while (length > 0) {
		size_t fragment = min_c(length, MAX_SECURE_SEND_FRAGMENT);
		params[0] = type;
		memcpy(params + 1, data, fragment);

		status_t status = intel_command(loader, INTEL_OP_SECURE_SEND,
			params, fragment + 1, true);
		if (status != B_OK)
			return status;

		data += fragment;
		length -= fragment;
	}

	return B_OK;
}


static status_t
read_firmware_file(const char* name, uint8** _data, size_t* _size)
{
	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};

	char path[B_PATH_NAME_LENGTH];
	int fd = -1;
	for (size_t i = 0; i < B_COUNT_OF(kDirectories) && fd < 0; i++) {
		if (find_directory(kDirectories[i], -1, false, path, sizeof(path))
				!= B_OK) {
			continue;
		}
		strlcat(path, "/firmware/h2generic/", sizeof(path));
		strlcat(path, name, sizeof(path));
		fd = open(path, O_RDONLY);
	}

	if (fd < 0) {
		ERROR("intel: firmware file %s not found\n", name);
		return B_ENTRY_NOT_FOUND;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	if (size <= 0 || size > MAX_FIRMWARE_SIZE) {
		close(fd);
		return B_BAD_DATA;
	}
	lseek(fd, 0, SEEK_SET);

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != size) {
		free(data);
		return B_IO_ERROR;
	}

	*_data = data;
	*_size = size;
	return B_OK;
}


static status_t
download_firmware(intel_loader* loader, const uint8* data, size_t size)
{
	status_t status = secure_send(loader, INTEL_FRAGMENT_INIT, data,
		SFI_CSS_HEADER_LENGTH);
	if (status == B_OK) {
		status = secure_send(loader, INTEL_FRAGMENT_PUBLIC_KEY,
			data + SFI_PUBLIC_KEY_OFFSET, SFI_PUBLIC_KEY_LENGTH);
	}
	if (status == B_OK) {
		status = secure_send(loader, INTEL_FRAGMENT_SIGNATURE,
			data + SFI_SIGNATURE_OFFSET, SFI_SIGNATURE_LENGTH);
	}
	if (status != B_OK) {
		ERROR("intel: firmware header rejected\n");
		return status;
	}

	// The data fragments must be a multiple of 4 bytes long. The file has
	// NOP commands that align the command groups.
	size_t offset = SFI_HEADER_LENGTH;
	size_t fragment = 0;
	while (offset + fragment < size) {
		if (offset + fragment + 3 > size)
			return B_BAD_DATA;

		fragment += 3 + data[offset + fragment + 2];
		if (offset + fragment > size)
			return B_BAD_DATA;

		if (fragment % 4 == 0) {
			status = secure_send(loader, INTEL_FRAGMENT_DATA, data + offset,
				fragment);
			if (status != B_OK)
				return status;

			offset += fragment;
			fragment = 0;
		}
	}

	return fragment == 0 ? B_OK : B_BAD_DATA;
}


static status_t
wait_for_vendor_event(intel_loader* loader, bool booting,
	bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	uint8 event[HCI_MAX_EVENT_SIZE];
	size_t length;

	while (booting ? !loader->booted : loader->downloadResult < 0) {
		status_t status = next_event(loader, event, &length, deadline);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


static void
write_ddc(intel_loader* loader, const char* name)
{
	uint8* data;
	size_t size;
	if (read_firmware_file(name, &data, &size) != B_OK)
		return;

	// Each record has a length byte, a 2 byte ID and a value. The length
	// does not include the length byte.
	size_t offset = 0;
	status_t status = B_OK;
	while (offset < size && status == B_OK) {
		size_t length = data[offset] + 1;
		if (length < 3 || length > 255 || offset + length > size) {
			status = B_BAD_DATA;
			break;
		}

		status = intel_command(loader, INTEL_OP_WRITE_DDC, data + offset,
			length, false);
		offset += length;
	}
	free(data);

	if (status != B_OK)
		ERROR("intel: cannot apply %s: %s\n", name, strerror(status));
	else
		ERROR("intel: applied %s\n", name);
}


static status_t
setup_controller(intel_loader* loader)
{
	intel_version version;
	status_t status = read_version(loader, &version);
	if (status != B_OK)
		return status;

	if (version.hw_platform != INTEL_HW_PLATFORM
		|| (version.hw_variant != 0x0b && version.hw_variant != 0x0c)) {
		ERROR("intel: hardware platform 0x%02x variant 0x%02x not "
			"supported\n", version.hw_platform, version.hw_variant);
		return B_NOT_SUPPORTED;
	}

	if (version.fw_variant == INTEL_FW_OPERATIONAL) {
		ERROR("intel: operational firmware build %u week %u %u is "
			"running\n", version.fw_build_num, version.fw_build_ww,
			2000 + version.fw_build_yy);
		return B_OK;
	}

	if (version.fw_variant != INTEL_FW_BOOTLOADER) {
		ERROR("intel: firmware variant 0x%02x not supported\n",
			version.fw_variant);
		return B_NOT_SUPPORTED;
	}

	intel_boot_params params;
	size_t length;
	status = intel_command(loader, INTEL_OP_READ_BOOT_PARAMS, NULL, 0, false,
		&params, sizeof(params), &length);
	if (status != B_OK)
		return status;
	if (length != sizeof(params))
		return B_BAD_DATA;

	// Without a Command Complete event for each fragment, the download
	// cannot be checked.
	if (params.limited_cce != 0) {
		ERROR("intel: boot loader with limited command complete events "
			"not supported\n");
		return B_NOT_SUPPORTED;
	}

	char name[32];
	snprintf(name, sizeof(name), "ibt-%u-%u.sfi", version.hw_variant,
		B_LENDIAN_TO_HOST_INT16(params.dev_revid));

	uint8* firmware;
	size_t size;
	status = read_firmware_file(name, &firmware, &size);
	if (status != B_OK)
		return status;
	if (size < SFI_HEADER_LENGTH) {
		free(firmware);
		return B_BAD_DATA;
	}

	ERROR("intel: loading %s\n", name);
	bigtime_t start = system_time();
	loader->downloadResult = -1;
	status = download_firmware(loader, firmware, size);
	free(firmware);
	if (status != B_OK) {
		ERROR("intel: firmware download failed: %s\n", strerror(status));
		return status;
	}

	status = wait_for_vendor_event(loader, false, kDownloadTimeout);
	if (status != B_OK || loader->downloadResult != 0) {
		ERROR("intel: firmware not accepted (%s, result %" B_PRId32 ")\n",
			strerror(status), loader->downloadResult);
		return status != B_OK ? status : B_IO_ERROR;
	}
	ERROR("intel: firmware downloaded in %" B_PRIdBIGTIME " ms\n",
		(system_time() - start) / 1000);

	// For these hardware variants the boot address is 0. The controller
	// sends no Command Complete event for the reset, only the bootup
	// event from the new firmware.
	intel_reset reset = { 0x00, 0x01, 0x00, 0x01, 0 };
	loader->booted = false;
	status = send_command(loader, INTEL_OP_RESET, &reset, sizeof(reset),
		false);
	if (status == B_OK)
		status = wait_for_vendor_event(loader, true, kBootTimeout);
	if (status != B_OK) {
		ERROR("intel: firmware did not boot: %s\n", strerror(status));
		return status;
	}

	// The controller works without the DDC settings, so this cannot fail.
	snprintf(name, sizeof(name), "ibt-%u-%u.ddc", version.hw_variant,
		B_LENDIAN_TO_HOST_INT16(params.dev_revid));
	write_ddc(loader, name);

	status = read_version(loader, &version);
	if (status != B_OK)
		return status;

	ERROR("intel: firmware build %u week %u %u is running\n",
		version.fw_build_num, version.fw_build_ww,
		2000 + version.fw_build_yy);
	return B_OK;
}


bool
intel_needs_firmware(const usb_device_descriptor* descriptor)
{
	if (descriptor->vendor_id != 0x8087)
		return false;

	for (size_t i = 0; i < B_COUNT_OF(kIntelDevices); i++) {
		if (descriptor->product_id == kIntelDevices[i])
			return true;
	}

	return false;
}


status_t
intel_load_firmware(bt_usb_dev* bdev)
{
	intel_loader* loader = (intel_loader*)calloc(1, sizeof(intel_loader));
	if (loader == NULL)
		return B_NO_MEMORY;

	loader->bdev = bdev;
	loader->downloadResult = -1;
	mutex_init(&loader->lock, "h2generic intel events");

	status_t status = B_OK;
	loader->eventSem = create_sem(0, "h2generic intel events");
	loader->bulkOutSem = create_sem(0, "h2generic intel bulk");
	if (loader->eventSem < 0 || loader->bulkOutSem < 0)
		status = B_NO_MORE_SEMS;

	if (status == B_OK)
		status = start_rx(loader);
	if (status == B_OK)
		status = setup_controller(loader);

	stop_rx(loader);

	// A receive callback can still use the loader, so keep it.
	if (atomic_get(&loader->pendingRx) > 0)
		return status != B_OK ? status : B_ERROR;

	if (loader->eventSem >= 0)
		delete_sem(loader->eventSem);
	if (loader->bulkOutSem >= 0)
		delete_sem(loader->bulkOutSem);
	mutex_destroy(&loader->lock);
	free(loader);

	return status;
}
