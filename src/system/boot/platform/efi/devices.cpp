/*
 * Copyright 2016-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/stage2.h>

#include <boot/net/Ethernet.h>
#include <boot/net/NetStack.h>
#include <boot/net/RemoteDisk.h>

#include "Header.h"

#include "efi_platform.h"
#include <efi/protocol/block-io.h>
#include <efi/protocol/loaded-image.h>
#include <util/convertutf.h>

#include "gpt.h"
#include "gpt_known_guids.h"


//#define TRACE_DEVICES
#ifdef TRACE_DEVICES
#   define TRACE(x...) dprintf("efi/devices: " x)
#else
#   define TRACE(x...) ;
#endif


static efi_guid BlockIoGUID = EFI_BLOCK_IO_PROTOCOL_GUID;


class EfiDevice : public Node
{
	public:
		EfiDevice(efi_block_io_protocol *blockIo);
		virtual ~EfiDevice();

		virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer,
			size_t bufferSize);
		virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer,
			size_t bufferSize) { return B_UNSUPPORTED; }
		virtual off_t Size() const {
			return (fBlockIo->Media->LastBlock + 1) * BlockSize(); }

		uint32 BlockSize() const { return fBlockIo->Media->BlockSize; }
	private:
		efi_block_io_protocol*		fBlockIo;
};


EfiDevice::EfiDevice(efi_block_io_protocol *blockIo)
	:
	fBlockIo(blockIo)
{
}


EfiDevice::~EfiDevice()
{
}


ssize_t
EfiDevice::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	TRACE("%s called. pos: %" B_PRIdOFF ", %p, %" B_PRIuSIZE "\n", __func__,
		pos, buffer, bufferSize);

	// Reads go through a page aligned bounce buffer, in bounded chunks.
	// This used to be a variable length array on the stack, sized after the
	// request, which overflowed the (small) firmware stack for large reads,
	// e.g. on the Raspberry Pi 4.
	static const size_t kBounceSize = 256 * 1024;
	static char* sBounceBuffer = NULL;
	if (sBounceBuffer == NULL) {
		efi_physical_addr address;
		if (kBootServices->AllocatePages(AllocateAnyPages, EfiLoaderData,
				kBounceSize / B_PAGE_SIZE, &address) != EFI_SUCCESS) {
			dprintf("%s: could not allocate bounce buffer\n", __func__);
			return B_NO_MEMORY;
		}
		sBounceBuffer = (char*)(addr_t)address;
	}

	const uint32 blockSize = BlockSize();
	if (blockSize == 0 || blockSize > kBounceSize)
		return B_ERROR;
	const size_t maxBlocks = kBounceSize / blockSize;

	size_t done = 0;
	while (done < bufferSize) {
		off_t current = pos + done;
		off_t block = current / blockSize;
		size_t offset = current % blockSize;
		size_t toCopy = bufferSize - done;
		size_t numBlocks = (offset + toCopy + blockSize - 1) / blockSize;
		if (numBlocks > maxBlocks) {
			numBlocks = maxBlocks;
			toCopy = numBlocks * blockSize - offset;
		}

		if (fBlockIo->ReadBlocks(fBlockIo, fBlockIo->Media->MediaId,
				block, numBlocks * blockSize, sBounceBuffer) != EFI_SUCCESS) {
			dprintf("%s: blockIo error reading from device!\n", __func__);
			return B_ERROR;
		}

		memcpy((char*)buffer + done, sBounceBuffer + offset, toCopy);
		done += toCopy;
	}

	return bufferSize;
}


static bool
device_contains_partition(EfiDevice *device, boot::Partition *partition)
{
	EFI::Header *header = (EFI::Header*)partition->content_cookie;
	if (header != NULL && header->InitCheck() == B_OK) {
		// check if device is GPT, and contains partition entry
		uint32 blockSize = device->BlockSize();
		gpt_table_header *deviceHeader =
			(gpt_table_header*)malloc(blockSize);
		ssize_t bytesRead = device->ReadAt(NULL, blockSize, deviceHeader,
			blockSize);
		if (bytesRead != (ssize_t)blockSize)
			return false;

		if (memcmp(deviceHeader, &header->TableHeader(),
				sizeof(gpt_table_header)) != 0)
			return false;

		// partition->cookie == int partition entry index
		uint32 index = (uint32)(addr_t)partition->cookie;
		uint32 size = sizeof(gpt_partition_entry) * (index + 1);
		gpt_partition_entry *entries = (gpt_partition_entry*)malloc(size);
		bytesRead = device->ReadAt(NULL,
			deviceHeader->entries_block * blockSize, entries, size);
		if (bytesRead != (ssize_t)size)
			return false;

		if (memcmp(&entries[index], &header->EntryAt(index),
				sizeof(gpt_partition_entry)) != 0)
			return false;

		for (size_t i = 0; i < sizeof(kTypeMap) / sizeof(struct type_map); ++i)
			if (strcmp(kTypeMap[i].type, BFS_NAME) == 0)
				if (kTypeMap[i].guid == header->EntryAt(index).partition_type)
					return true;

		// Our partition has an EFI header, but we couldn't find one, so bail
		return false;
	}

	if ((partition->offset + partition->size) <= device->Size())
			return true;

	return false;
}


static efi_guid sLoadedImageGUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static efi_guid sLoadedImageDevicePathGUID
	= EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID;
static efi_guid sDevicePathGUID = EFI_DEVICE_PATH_PROTOCOL_GUID;


/*!	Whether the load options ask for a network boot with an "ip=" token.

	That is how a network boot is requested when the loader itself came from
	local media; it is also what EFIEthernetInterface::Init() looks for, and
	without it that function gives up anyway.
*/
static bool
load_options_request_network(void)
{
	efi_loaded_image_protocol *loadedImage = NULL;
	if (kBootServices->HandleProtocol(kImage, &sLoadedImageGUID,
			(void**)&loadedImage) != EFI_SUCCESS || loadedImage == NULL
		|| loadedImage->LoadOptions == NULL
		|| loadedImage->LoadOptionsSize == 0) {
		return false;
	}

	char buffer[256];
	buffer[0] = '\0';
	utf16le_to_utf8((uint16*)loadedImage->LoadOptions,
		loadedImage->LoadOptionsSize / 2, buffer, sizeof(buffer));

	for (const char *token = buffer; *token != '\0'; token++) {
		if ((token == buffer || token[-1] == ' ')
			&& strncmp(token, "ip=", 3) == 0) {
			return true;
		}
	}

	return false;
}


static efi_device_path_protocol*
device_path_for(efi_handle handle)
{
	efi_device_path_protocol *path = NULL;
	if (handle == NULL || kBootServices->HandleProtocol(handle,
			&sDevicePathGUID, (void**)&path) != EFI_SUCCESS) {
		return NULL;
	}

	return path;
}


static size_t
device_path_length(efi_device_path_protocol *path)
{
	efi_device_path_protocol *node = path;
	while (node->Type != DEVICE_PATH_END
		&& node->Length >= sizeof(efi_device_path_protocol)) {
		node = (efi_device_path_protocol*)(((uint8*)node) + node->Length);
	}

	return (uint8*)node - (uint8*)path;
}


/*!	Whether \a path names the disk, or something on it, that \a prefix names.

	The device path of a partition starts with the path of the disk holding
	it, so this says whether the loader came off that disk.
*/
static bool
device_path_starts_with(efi_device_path_protocol *path,
	efi_device_path_protocol *prefix)
{
	if (path == NULL || prefix == NULL)
		return false;

	size_t prefixLength = device_path_length(prefix);
	return prefixLength > 0 && device_path_length(path) >= prefixLength
		&& memcmp(prefix, path, prefixLength) == 0;
}


/*!	Whether this loader was itself loaded over the network. */
static bool
booted_from_network(void)
{
	efi_device_path_protocol *devicePath = NULL;
	if (kBootServices->HandleProtocol(kImage, &sLoadedImageDevicePathGUID,
			(void**)&devicePath) != EFI_SUCCESS || devicePath == NULL) {
		return false;
	}

	for (; devicePath->Length >= sizeof(efi_device_path_protocol);
		devicePath = (efi_device_path_protocol*)(((uint8*)devicePath)
			+ devicePath->Length)) {
		if (devicePath->Type == DEVICE_PATH_END)
			break;
		if (devicePath->Type != DEVICE_PATH_MESSAGING)
			continue;
		if (devicePath->SubType == DEVICE_PATH_MESSAGING_MAC
			|| devicePath->SubType == DEVICE_PATH_MESSAGING_IPV4
			|| devicePath->SubType == DEVICE_PATH_MESSAGING_IPV6) {
			return true;
		}
	}

	return false;
}


status_t
platform_add_boot_device(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	efi_block_io_protocol *blockIo;
	size_t memSize = 0;

	// Which disk this loader came off. Every disk the firmware knows gets
	// added below and the first one carrying a Haiku system wins, so on a
	// machine that already has Haiku installed the boot medium has to come
	// first -- otherwise booting a USB stick quietly starts the loader on
	// the installed volume instead, and the kernel is then told to mount a
	// partition it cannot match.
	efi_loaded_image_protocol *loadedImage = NULL;
	efi_device_path_protocol *bootDevicePath = NULL;
	if (kBootServices->HandleProtocol(kImage, &sLoadedImageGUID,
			(void**)&loadedImage) == EFI_SUCCESS && loadedImage != NULL)
		bootDevicePath = device_path_for(loadedImage->DeviceHandle);

	// If the bootloader was started from the network, add network booting as
	// the first entry if available.
	//
	// Only when the network was actually asked for: bringing the stack up
	// means locating and starting the Simple Network Protocol, and on Apple's
	// EFI that leaves the firmware unable to answer any Block I/O lookup
	// afterwards -- LocateProtocol for EFI_BLOCK_IO_PROTOCOL never returns --
	// so a Mac booting from USB hangs before it ever looks at the stick.
	// EFIEthernetInterface::Init() gives up without an "ip=" load option in
	// any case, so nothing that used to work is skipped here.
	status_t error = B_ERROR;
	if (booted_from_network() || load_options_request_network())
		error = net_stack_init();
	if (error != B_OK) {
		TRACE("Can't init network...\n");
	} else {
		TRACE("Network is initialized! Search for remote disk...\n");
		RemoteDisk *remoteDisk = RemoteDisk::FindAnyRemoteDisk();
		if (remoteDisk != NULL) {
			devicesList->Add(remoteDisk);
		}
	}

	// Read to zero sized buffer to get memory needed for handles
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize, 0)
			!= EFI_BUFFER_TOO_SMALL)
		panic("Cannot read size of block device handles!");

	uint32 noOfHandles = memSize / sizeof(efi_handle);

	efi_handle handles[noOfHandles];
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize,
			handles) != EFI_SUCCESS)
		panic("Failed to locate block devices!");

	// All block devices has one for the disk and one per partition
	// There is a special case for a device with one fixed partition
	// But we probably do not care about booting on that kind of device
	// So find all disk block devices and let Haiku do partition scan
	for (uint32 n = 0; n < noOfHandles; n++) {
		if (kBootServices->HandleProtocol(handles[n], &BlockIoGUID,
				(void**)&blockIo) != EFI_SUCCESS)
			panic("Cannot get block device handle!");

		TRACE("%s: %p: present: %s, logical: %s, removeable: %s, "
			"blocksize: %" PRIu32 ", lastblock: %" PRIu64 "\n",
			__func__, blockIo,
			blockIo->Media->MediaPresent ? "true" : "false",
			blockIo->Media->LogicalPartition ? "true" : "false",
			blockIo->Media->RemovableMedia ? "true" : "false",
			blockIo->Media->BlockSize, blockIo->Media->LastBlock);

		if (!blockIo->Media->MediaPresent
			|| blockIo->Media->LogicalPartition
			|| blockIo->Media->BlockSize == 0)
			continue;

		// The qemu flash device with a 256K block sizes sometime show up
		// in edk2. If flash is unconfigured, bad things happen on arm.
		// edk2 bug: https://bugzilla.tianocore.org/show_bug.cgi?id=2856
		// We're not ready for flash devices in efi, so skip anything odd.
		if (blockIo->Media->BlockSize > 8192)
			continue;

		EfiDevice *device = new(std::nothrow)EfiDevice(blockIo);
		if (device == NULL)
			panic("Can't allocate memory for block devices!");

		bool isBootDisk = device_path_starts_with(bootDevicePath,
			device_path_for(handles[n]));
		if (isBootDisk)
			TRACE("%s: booted off this disk\n", __func__);
		devicesList->Insert(device, !isBootDisk);
	}

	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_add_block_devices(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	//TODO: Currently we add all in platform_add_boot_device
	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_get_boot_partitions(struct stage2_args *args, Node *bootDevice,
		NodeList *partitions, NodeList *bootPartitions)
{
	NodeIterator iterator = partitions->GetIterator();
	boot::Partition *partition = NULL;
	while ((partition = (boot::Partition*)iterator.Next()) != NULL) {
		if (device_contains_partition((EfiDevice*)bootDevice, partition)) {
			bootPartitions->Insert(partition);
		}
	}

	return bootPartitions->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_register_boot_device(Node *device, disk_identifier* defaultDiskID)
{
	TRACE("%s: called\n", __func__);

	// TODO is there no better way to identify network boot?
	char buffer[11];
	device->GetName(buffer, sizeof(buffer));
	TRACE("Booting from %s\n", buffer);

	if (strcmp(buffer, "RemoteDisk") == 0) {
		TRACE("Booting from network\n");
		RemoteDisk* remoteDisk = static_cast<RemoteDisk*>(device);
		// Tell the kernel we're booting from network and forward the network
		// configuration
		auto ethernetInterface = NetStack::Default()->GetEthernetInterface();
		if (gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_NET)
			|| gBootParams.AddInt64("client MAC",
				ethernetInterface->MACAddress().ToUInt64()) != B_OK
			|| gBootParams.AddInt32("client IP",
				ethernetInterface->IPAddress()) != B_OK
			|| gBootParams.AddInt32("server IP", remoteDisk->ServerIPAddress())
				!= B_OK
			|| gBootParams.AddInt32("server port", remoteDisk->ServerPort())
				!= B_OK) {
			return B_NO_MEMORY;
		}
	} else {
		// ...HARD_DISK, as we pick partition and have checksum (no need to use _CD)
		gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_HARD_DISK);
	}

	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE,
		defaultDiskID, sizeof(disk_identifier));

	return B_OK;
}


void
platform_cleanup_devices()
{
}
