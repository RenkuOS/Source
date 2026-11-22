/*
 * Copyright 2006-2018, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 *		Adrien Destugues <pulkomandy@pulkomandy.tk>
 */

#include "usb_raw.h"

#include <util/AutoLock.h>
#include <AutoDeleter.h>
#include <KernelExport.h>
#include <Drivers.h>
#include <algorithm>
#include <lock.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <kernel.h>


//#define TRACE_USB_RAW
#ifdef TRACE_USB_RAW
#define TRACE(x)	dprintf x
#else
#define TRACE(x)	/* nothing */
#endif

#define DRIVER_NAME		"usb_raw"
#define DEVICE_NAME		"bus/usb/raw"

struct raw_stream;

// Streams of one device at a time, one per endpoint: a camera streams video
// and sound on two endpoints of the same device.
#define MAX_STREAMS				4

typedef struct {
	usb_device			device;
	mutex				lock;
	mutex				stream_lock;
	raw_stream			*streams[MAX_STREAMS];
	uint32				reference_count;

	char				name[64];
	void				*link;

	sem_id				notify;
	status_t			status;
	size_t				actual_length;
} raw_device;

int32 api_version = B_CUR_DRIVER_API_VERSION;
static usb_module_info *gUSBModule = NULL;
static raw_device *gDeviceList = NULL;
static uint32 gDeviceCount = 0;
static mutex gDeviceListLock;
static char **gDeviceNames = NULL;

static status_t
usb_raw_device_added(usb_device newDevice, void **cookie)
{
	TRACE((DRIVER_NAME": device_added(0x%08" B_PRIx32 ")\n", newDevice));
	raw_device *device = (raw_device *)malloc(sizeof(raw_device));

	mutex_init(&device->lock, "usb_raw device lock");
	mutex_init(&device->stream_lock, "usb_raw stream lock");
	for (int32 i = 0; i < MAX_STREAMS; i++)
		device->streams[i] = NULL;

	device->notify = create_sem(0, "usb_raw callback notify");
	if (device->notify < B_OK) {
		mutex_destroy(&device->lock);
		mutex_destroy(&device->stream_lock);
		free(device);
		return B_NO_MORE_SEMS;
	}

	char deviceName[64];
	memcpy(deviceName, &newDevice, sizeof(usb_device));
	if (gUSBModule->usb_ioctl('DNAM', deviceName, sizeof(deviceName)) >= B_OK) {
		snprintf(device->name, sizeof(device->name), "bus/usb/%s", deviceName);
	} else {
		snprintf(device->name, sizeof(device->name), "bus/usb/%08" B_PRIx32,
			newDevice);
	}

	device->device = newDevice;
	device->reference_count = 0;

	mutex_lock(&gDeviceListLock);
	device->link = (void *)gDeviceList;
	gDeviceList = device;
	gDeviceCount++;
	mutex_unlock(&gDeviceListLock);

	TRACE((DRIVER_NAME": new device: 0x%p\n", device));
	*cookie = (void *)device;
	return B_OK;
}


static void usb_raw_stream_stop(raw_device *device);
static void usb_raw_stream_callback(void *cookie, status_t status, void *data,
	size_t actualLength);


static status_t
usb_raw_device_removed(void *cookie)
{
	TRACE((DRIVER_NAME": device_removed(0x%p)\n", cookie));
	raw_device *device = (raw_device *)cookie;

	// the stream's transfers have callbacks to be called, too
	usb_raw_stream_stop(device);

	// cancel all pending transfers to make sure no one keeps waiting forever
	// in syscalls. This has to happen here: once the device is freed, the
	// stack cancels what is left by force, without calling the callbacks, and
	// an ioctl waiting for one would never return while holding the device
	// lock. That includes control requests on the default pipe.
	gUSBModule->cancel_queued_requests(device->device);

	const usb_configuration_info *configurationInfo =
		gUSBModule->get_configuration(device->device);
	if (configurationInfo != NULL) {
		for (size_t i = 0; i < configurationInfo->interface_count; i++) {
			// An interface the device described no alternate for has no
			// active one.
			struct usb_interface_info* interface
				= configurationInfo->interface[i].active;
			if (interface == NULL)
				continue;

			for (size_t j = 0; j < interface->endpoint_count; j++) {
				gUSBModule->cancel_queued_transfers(
					interface->endpoint[j].handle);
			}
		}
	}

	mutex_lock(&gDeviceListLock);
	if (gDeviceList == device) {
		gDeviceList = (raw_device *)device->link;
	} else {
		raw_device *element = gDeviceList;
		while (element) {
			if (element->link == device) {
				element->link = device->link;
				break;
			}

			element = (raw_device *)element->link;
		}
	}
	gDeviceCount--;
	mutex_unlock(&gDeviceListLock);

	device->device = 0;
	if (device->reference_count == 0) {
		mutex_lock(&device->lock);
		mutex_destroy(&device->lock);
		mutex_destroy(&device->stream_lock);
		delete_sem(device->notify);
		free(device);
	}

	return B_OK;
}


//
//#pragma mark -
//


static const usb_configuration_info *
usb_raw_get_configuration(raw_device *device, uint32 configIndex,
	status_t *status)
{
	const usb_configuration_info *result = gUSBModule->get_nth_configuration(
		device->device, configIndex);
	if (result == NULL) {
		*status = B_USB_RAW_STATUS_INVALID_CONFIGURATION;
		return NULL;
	}

	return result;
}


static const usb_interface_info *
usb_raw_get_interface(raw_device *device, uint32 configIndex,
	uint32 interfaceIndex, uint32 alternateIndex, status_t *status)
{
	const usb_configuration_info *configurationInfo
		= usb_raw_get_configuration(device, configIndex, status);
	if (configurationInfo == NULL)
		return NULL;

	if (interfaceIndex >= configurationInfo->interface_count) {
		*status = B_USB_RAW_STATUS_INVALID_INTERFACE;
		return NULL;
	}

	const usb_interface_info *result = NULL;
	if (alternateIndex == B_USB_RAW_ACTIVE_ALTERNATE)
		result = configurationInfo->interface[interfaceIndex].active;
	else {
		const usb_interface_list *interfaceList =
			&configurationInfo->interface[interfaceIndex];
		if (alternateIndex >= interfaceList->alt_count) {
			*status = B_USB_RAW_STATUS_INVALID_INTERFACE;
			return NULL;
		}

		result = &interfaceList->alt[alternateIndex];
	}

	return result;
}


//
//#pragma mark -
//


static status_t
usb_raw_open(const char *name, uint32 flags, void **cookie)
{
	TRACE((DRIVER_NAME": open()\n"));
	mutex_lock(&gDeviceListLock);
	raw_device *element = gDeviceList;
	while (element) {
		if (strcmp(name, element->name) == 0) {
			element->reference_count++;
			*cookie = element;
			mutex_unlock(&gDeviceListLock);
			return B_OK;
		}

		element = (raw_device *)element->link;
	}

	mutex_unlock(&gDeviceListLock);
	return B_NAME_NOT_FOUND;
}


static status_t
usb_raw_close(void *cookie)
{
	TRACE((DRIVER_NAME": close()\n"));
	return B_OK;
}


static status_t
usb_raw_free(void *cookie)
{
	TRACE((DRIVER_NAME": free()\n"));
	mutex_lock(&gDeviceListLock);

	raw_device *device = (raw_device *)cookie;
	device->reference_count--;
	if (device->device != 0 && device->reference_count == 0) {
		// nobody is left to read from it
		usb_raw_stream_stop(device);
	}
	if (device->device == 0 && device->reference_count == 0) {
		// the device is gone and this was its last user
		mutex_lock(&device->lock);
		mutex_destroy(&device->lock);
		mutex_destroy(&device->stream_lock);
		delete_sem(device->notify);
		free(device);
	}

	mutex_unlock(&gDeviceListLock);
	return B_OK;
}


static void
usb_raw_callback(void *cookie, status_t status, void *data, size_t actualLength)
{
	TRACE((DRIVER_NAME": callback()\n"));
	raw_device *device = (raw_device *)cookie;

	switch (status) {
		case B_OK:
			device->status = B_USB_RAW_STATUS_SUCCESS;
			break;
		case B_TIMED_OUT:
			device->status = B_USB_RAW_STATUS_TIMEOUT;
			break;
		case B_CANCELED:
			device->status = B_USB_RAW_STATUS_ABORTED;
			break;
		case B_DEV_CRC_ERROR:
			device->status = B_USB_RAW_STATUS_CRC_ERROR;
			break;
		case B_DEV_STALLED:
			device->status = B_USB_RAW_STATUS_STALLED;
			break;
		default:
			device->status = B_USB_RAW_STATUS_FAILED;
			break;
	}

	device->actual_length = actualLength;
	release_sem(device->notify);
}


//
//#pragma mark - isochronous streams
//


// A stream that nobody reads from is stopped after this long, so that a
// program that died does not leave the device streaming for ever.
#define STREAM_IDLE_TIMEOUT		5000000LL
#define STREAM_MAX_ERRORS		64

struct raw_stream_transfer {
	raw_stream					*stream;
	uint8						*data;
	usb_iso_packet_descriptor	*descriptors;
};

struct raw_stream {
	usb_pipe					pipe;
	uint32						interface;
	uint32						packet_size;
	uint32						packets_per_transfer;
	uint32						transfer_count;
	raw_stream_transfer			*transfers;

	mutex						lock;
	uint8						*buffer;
	size_t						buffer_size;
	size_t						buffer_head;
	size_t						buffer_used;
	bool						gap;

	bool						running;
	int32						pending;
	uint32						errors;
	bigtime_t					last_read;

	sem_id						data_sem;
	int32						waiting;
	int32						readers;
};


static void
usb_raw_stream_store(raw_stream *stream, const void *_data, size_t length)
{
	const uint8 *data = (const uint8 *)_data;
	size_t position = (stream->buffer_head + stream->buffer_used)
		% stream->buffer_size;
	size_t first = min_c(length, stream->buffer_size - position);
	memcpy(stream->buffer + position, data, first);
	if (first < length)
		memcpy(stream->buffer, data + first, length - first);
	stream->buffer_used += length;
}


static void
usb_raw_stream_fetch(raw_stream *stream, void *_data, size_t offset,
	size_t length)
{
	uint8 *data = (uint8 *)_data;
	size_t position = (stream->buffer_head + offset) % stream->buffer_size;
	size_t first = min_c(length, stream->buffer_size - position);
	memcpy(data, stream->buffer + position, first);
	if (first < length)
		memcpy(data + first, stream->buffer, length - first);
}


static status_t
usb_raw_stream_queue(raw_stream_transfer *transfer)
{
	raw_stream *stream = transfer->stream;
	for (uint32 i = 0; i < stream->packets_per_transfer; i++) {
		transfer->descriptors[i].request_length = stream->packet_size;
		transfer->descriptors[i].actual_length = 0;
		transfer->descriptors[i].status = B_OK;
	}

	return gUSBModule->queue_isochronous(stream->pipe, transfer->data,
		stream->packet_size * stream->packets_per_transfer,
		transfer->descriptors, stream->packets_per_transfer, NULL,
		USB_ISO_ASAP, usb_raw_stream_callback, transfer);
}


static void
usb_raw_stream_callback(void *cookie, status_t status, void *data,
	size_t actualLength)
{
	raw_stream_transfer *transfer = (raw_stream_transfer *)cookie;
	raw_stream *stream = transfer->stream;

	MutexLocker locker(stream->lock);

	if (status != B_CANCELED && stream->running) {
		size_t offset = 0;
		for (uint32 i = 0; i < stream->packets_per_transfer; i++) {
			const usb_iso_packet_descriptor &descriptor
				= transfer->descriptors[i];
			size_t length = descriptor.actual_length;
			if (length > stream->packet_size)
				length = stream->packet_size;

			if (length > 0) {
				usb_raw_stream_packet header;
				if (stream->buffer_used + sizeof(header) + length
						> stream->buffer_size) {
					stream->gap = true;
				} else {
					header.length = length;
					header.flags = 0;
					if (descriptor.status != B_OK)
						header.flags |= B_USB_RAW_STREAM_PACKET_ERROR;
					if (stream->gap)
						header.flags |= B_USB_RAW_STREAM_PACKET_GAP;
					stream->gap = false;
					usb_raw_stream_store(stream, &header, sizeof(header));
					usb_raw_stream_store(stream, transfer->data + offset,
						length);
				}
			}

			offset += stream->packet_size;
		}
	}

	bool requeue = stream->running && status != B_CANCELED;
	if (status == B_OK)
		stream->errors = 0;
	else if (++stream->errors > STREAM_MAX_ERRORS)
		requeue = false;
	if (system_time() - stream->last_read > STREAM_IDLE_TIMEOUT)
		requeue = false;

	if (requeue && usb_raw_stream_queue(transfer) != B_OK)
		requeue = false;

	if (!requeue) {
		stream->pending--;
		if (stream->pending == 0)
			stream->running = false;
	}

	if (stream->waiting > 0 && (stream->buffer_used > 0 || !stream->running)) {
		stream->waiting--;
		release_sem_etc(stream->data_sem, 1, B_DO_NOT_RESCHEDULE);
	}
}


static void
usb_raw_stream_free(raw_stream *stream)
{
	if (stream->transfers != NULL) {
		for (uint32 i = 0; i < stream->transfer_count; i++) {
			free(stream->transfers[i].data);
			free(stream->transfers[i].descriptors);
		}
		free(stream->transfers);
	}
	free(stream->buffer);
	if (stream->data_sem >= 0)
		delete_sem(stream->data_sem);
	mutex_destroy(&stream->lock);
	free(stream);
}


static void
usb_raw_stream_stop_slot(raw_device *device, int32 slot)
{
	mutex_lock(&device->stream_lock);
	raw_stream *stream = device->streams[slot];
	device->streams[slot] = NULL;
	mutex_unlock(&device->stream_lock);

	if (stream == NULL)
		return;

	mutex_lock(&stream->lock);
	stream->running = false;
	bool pending = stream->pending > 0;
	mutex_unlock(&stream->lock);

	// this calls the callbacks of what is still queued
	if (pending)
		gUSBModule->cancel_queued_transfers(stream->pipe);

	// Wait for the transfers and for readers to be gone. The callbacks are
	// normally called by the cancel above; in case one is still on its way,
	// give it time.
	bool done = false;
	for (int32 tries = 0; tries < 200; tries++) {
		mutex_lock(&stream->lock);
		done = stream->pending == 0 && stream->readers == 0;
		while (stream->waiting > 0) {
			stream->waiting--;
			release_sem_etc(stream->data_sem, 1, B_DO_NOT_RESCHEDULE);
		}
		mutex_unlock(&stream->lock);
		if (done)
			break;
		snooze(10000);
	}

	if (!done) {
		// Something still refers to the stream: leaking it is the lesser evil.
		dprintf(DRIVER_NAME ": stream still in use, not freeing it\n");
		return;
	}

	usb_raw_stream_free(stream);
}


//!	Stops every stream of the device.
static void
usb_raw_stream_stop(raw_device *device)
{
	for (int32 i = 0; i < MAX_STREAMS; i++)
		usb_raw_stream_stop_slot(device, i);
}


//!	Stops the streams of one interface, whose pipes are about to go away.
static void
usb_raw_stream_stop_interface(raw_device *device, uint32 interface)
{
	for (int32 i = 0; i < MAX_STREAMS; i++) {
		mutex_lock(&device->stream_lock);
		bool match = device->streams[i] != NULL
			&& device->streams[i]->interface == interface;
		mutex_unlock(&device->stream_lock);
		if (match)
			usb_raw_stream_stop_slot(device, i);
	}
}


/*!	The slot of the stream of \a pipe, or with a NULL \a pipe the first
	stream (the commands that name no endpoint). -1 when there is none. The
	caller holds the stream lock of the device.
*/
static int32
usb_raw_stream_slot(raw_device *device, usb_pipe pipe)
{
	for (int32 i = 0; i < MAX_STREAMS; i++) {
		raw_stream *stream = device->streams[i];
		if (stream != NULL && (pipe == 0 || stream->pipe == pipe))
			return i;
	}
	return -1;
}


static status_t
usb_raw_stream_start(raw_device *device, uint32 interface, usb_pipe pipe,
	uint32 packetSize, uint32 packetsPerTransfer, uint32 transferCount,
	uint32 bufferSize)
{
	if (packetSize == 0 || packetSize > 3 * 1024 || packetsPerTransfer == 0
		|| packetsPerTransfer > 64 || transferCount == 0 || transferCount > 6
		|| bufferSize > 64 * 1024 * 1024) {
		return B_BAD_VALUE;
	}

	// room for at least what the queued transfers can deliver at once
	size_t minimum = (size_t)(packetSize + sizeof(usb_raw_stream_packet))
		* packetsPerTransfer * transferCount * 2;
	if (bufferSize < minimum)
		bufferSize = minimum;

	// A new stream replaces the endpoint's old one, and needs a free slot.
	mutex_lock(&device->stream_lock);
	int32 old = usb_raw_stream_slot(device, pipe);
	mutex_unlock(&device->stream_lock);
	if (old >= 0)
		usb_raw_stream_stop_slot(device, old);

	raw_stream *stream = (raw_stream *)calloc(1, sizeof(raw_stream));
	if (stream == NULL)
		return B_NO_MEMORY;

	mutex_init(&stream->lock, "usb_raw stream");
	stream->pipe = pipe;
	stream->interface = interface;
	stream->packet_size = packetSize;
	stream->packets_per_transfer = packetsPerTransfer;
	stream->transfer_count = transferCount;
	stream->buffer_size = bufferSize;
	stream->last_read = system_time();
	stream->data_sem = create_sem(0, "usb_raw stream data");
	stream->buffer = (uint8 *)malloc(bufferSize);
	stream->transfers = (raw_stream_transfer *)calloc(transferCount,
		sizeof(raw_stream_transfer));

	bool allocated = stream->data_sem >= 0 && stream->buffer != NULL
		&& stream->transfers != NULL;
	for (uint32 i = 0; allocated && i < transferCount; i++) {
		raw_stream_transfer &transfer = stream->transfers[i];
		transfer.stream = stream;
		transfer.data = (uint8 *)malloc(packetSize * packetsPerTransfer);
		transfer.descriptors = (usb_iso_packet_descriptor *)malloc(
			sizeof(usb_iso_packet_descriptor) * packetsPerTransfer);
		if (transfer.data == NULL || transfer.descriptors == NULL)
			allocated = false;
	}

	if (!allocated) {
		usb_raw_stream_free(stream);
		return B_NO_MEMORY;
	}

	// Take a free slot in one go: a camera's video and sound streams may be
	// started at the same time from two threads.
	mutex_lock(&device->stream_lock);
	int32 slot = -1;
	for (int32 i = 0; i < MAX_STREAMS; i++) {
		if (device->streams[i] == NULL) {
			slot = i;
			device->streams[i] = stream;
			break;
		}
	}
	mutex_unlock(&device->stream_lock);
	if (slot < 0) {
		usb_raw_stream_free(stream);
		return B_BUSY;
	}

	mutex_lock(&stream->lock);
	stream->running = true;
	status_t status = B_OK;
	for (uint32 i = 0; i < transferCount; i++) {
		status = usb_raw_stream_queue(&stream->transfers[i]);
		if (status != B_OK)
			break;
		stream->pending++;
	}
	if (stream->pending == 0)
		stream->running = false;
	mutex_unlock(&stream->lock);

	if (status != B_OK) {
		dprintf(DRIVER_NAME ": queueing a stream transfer failed: %s\n",
			strerror(status));
		usb_raw_stream_stop_slot(device, slot);
		return status;
	}

	return B_OK;
}


/*!	Copies whole packets into the caller's buffer, waiting up to \a timeout
	for the first one.
*/
static status_t
usb_raw_stream_read(raw_device *device, usb_pipe pipe, void *userData,
	size_t *_length, bigtime_t timeout)
{
	size_t capacity = *_length;
	*_length = 0;

	mutex_lock(&device->stream_lock);
	int32 slot = usb_raw_stream_slot(device, pipe);
	raw_stream *stream = slot >= 0 ? device->streams[slot] : NULL;
	if (stream == NULL) {
		mutex_unlock(&device->stream_lock);
		return B_NO_INIT;
	}
	mutex_lock(&stream->lock);
	if (stream->readers > 0) {
		// one reader at a time
		mutex_unlock(&stream->lock);
		mutex_unlock(&device->stream_lock);
		return B_BUSY;
	}
	stream->readers++;
	mutex_unlock(&device->stream_lock);

	status_t status = B_OK;
	size_t total = 0;
	bigtime_t end = system_time() + timeout;

	while (true) {
		stream->last_read = system_time();

		while (stream->buffer_used >= sizeof(usb_raw_stream_packet)) {
			usb_raw_stream_packet header;
			usb_raw_stream_fetch(stream, &header, 0, sizeof(header));
			size_t size = sizeof(header) + header.length;
			if (total + size > capacity)
				break;

			// The data cannot be copied to userland with the lock held (the
			// memory may have to be paged in), so take it out in pieces.
			uint8 chunk[512];
			size_t offset = 0;
			bool failed = false;
			mutex_unlock(&stream->lock);
			while (offset < size) {
				size_t length = min_c(sizeof(chunk), size - offset);
				mutex_lock(&stream->lock);
				usb_raw_stream_fetch(stream, chunk, offset, length);
				mutex_unlock(&stream->lock);
				if (user_memcpy((uint8 *)userData + total + offset, chunk,
						length) != B_OK) {
					failed = true;
					break;
				}
				offset += length;
			}
			mutex_lock(&stream->lock);
			if (failed) {
				status = B_BAD_ADDRESS;
				break;
			}

			stream->buffer_head = (stream->buffer_head + size)
				% stream->buffer_size;
			stream->buffer_used -= size;
			total += size;
		}

		if (total > 0 || status != B_OK)
			break;
		if (stream->buffer_used >= sizeof(usb_raw_stream_packet)) {
			// the caller's buffer cannot even hold one packet
			status = B_BUFFER_OVERFLOW;
			break;
		}
		if (!stream->running) {
			status = B_DEV_NOT_READY;
			break;
		}

		stream->waiting++;
		mutex_unlock(&stream->lock);
		status = acquire_sem_etc(stream->data_sem, 1,
			B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, end);
		mutex_lock(&stream->lock);
		if (status != B_OK) {
			// nobody released the semaphore for us
			if (stream->waiting > 0)
				stream->waiting--;
			else
				acquire_sem_etc(stream->data_sem, 1, B_RELATIVE_TIMEOUT, 0);
			break;
		}
	}

	stream->readers--;
	mutex_unlock(&stream->lock);

	*_length = total;
	return status;
}


//!	The pipe of an active endpoint, 0 when there is none.
static usb_pipe
usb_raw_stream_pipe(raw_device *device, uint32 interface, uint32 endpoint)
{
	const usb_configuration_info *configurationInfo
		= gUSBModule->get_configuration(device->device);
	if (configurationInfo == NULL
		|| interface >= configurationInfo->interface_count)
		return 0;
	const usb_interface_info *interfaceInfo
		= configurationInfo->interface[interface].active;
	if (interfaceInfo == NULL || endpoint >= interfaceInfo->endpoint_count)
		return 0;
	return interfaceInfo->endpoint[endpoint].handle;
}


static status_t
usb_raw_ioctl(void *cookie, uint32 op, void *buffer, size_t length)
{
	TRACE((DRIVER_NAME": ioctl\n"));
	raw_device *device = (raw_device *)cookie;
	if (device->device == 0)
		return B_DEV_NOT_READY;

	usb_raw_command command;
	if (length < sizeof(command.version.status))
		return B_BUFFER_OVERFLOW;
	if (!IS_USER_ADDRESS(buffer)
		|| user_memcpy(&command, buffer, min_c(length, sizeof(command)))
			!= B_OK) {
		return B_BAD_ADDRESS;
	}

	command.version.status = B_USB_RAW_STATUS_ABORTED;
	status_t status = B_DEV_INVALID_IOCTL;

	switch (op) {
		case B_USB_RAW_COMMAND_GET_VERSION:
		{
			command.version.status = B_USB_RAW_PROTOCOL_VERSION;
			status = B_OK;
			break;
		}

		case B_USB_RAW_COMMAND_GET_DEVICE_DESCRIPTOR:
		{
			if (length < sizeof(command.device))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_device_descriptor *deviceDescriptor =
				gUSBModule->get_device_descriptor(device->device);
			if (deviceDescriptor == NULL)
				return B_OK;

			if (!IS_USER_ADDRESS(command.device.descriptor)
				|| user_memcpy(command.device.descriptor, deviceDescriptor,
					sizeof(usb_device_descriptor)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.device.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR_ETC:
		{
			if (op == B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR_ETC
					&& length < sizeof(command.config_etc))
				return B_BUFFER_OVERFLOW;

			if (length < sizeof(command.config))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device, command.config.config_index,
					&command.config.status);
			if (configurationInfo == NULL)
				break;

			size_t sizeToCopy = sizeof(usb_configuration_descriptor);
			if (op == B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR_ETC) {
				sizeToCopy = std::min(command.config_etc.length,
					(size_t)configurationInfo->descr->total_length);
			}

			if (!IS_USER_ADDRESS(command.config.descriptor)
				|| user_memcpy(command.config.descriptor,
					configurationInfo->descr, sizeToCopy) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.config.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_ALT_INTERFACE_COUNT:
		case B_USB_RAW_COMMAND_GET_ACTIVE_ALT_INTERFACE_INDEX:
		{
			if (length < sizeof(command.alternate))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device,
					command.alternate.config_index,
					&command.alternate.status);
			if (configurationInfo == NULL)
				break;

			if (command.alternate.interface_index
				>= configurationInfo->interface_count) {
				command.alternate.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			const usb_interface_list *interfaceList
				= &configurationInfo->interface[
					command.alternate.interface_index];
			if (op == B_USB_RAW_COMMAND_GET_ALT_INTERFACE_COUNT) {
				command.alternate.alternate_info = interfaceList->alt_count;
			} else {
				for (size_t i = 0; i < interfaceList->alt_count; i++) {
					if (&interfaceList->alt[i] == interfaceList->active) {
						command.alternate.alternate_info = i;
						break;
					}
				}
			}

			command.alternate.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR_ETC:
		{
			const usb_interface_info *interfaceInfo = NULL;
			status = B_OK;
			if (op == B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR) {
				if (length < sizeof(command.interface))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.interface.config_index,
					command.interface.interface_index,
					B_USB_RAW_ACTIVE_ALTERNATE,
					&command.interface.status);
			} else {
				if (length < sizeof(command.interface_etc))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.interface_etc.config_index,
					command.interface_etc.interface_index,
					command.interface_etc.alternate_index,
					&command.interface_etc.status);
			}

			if (interfaceInfo == NULL)
				break;

			if (!IS_USER_ADDRESS(command.interface.descriptor)
				|| user_memcpy(command.interface.descriptor, interfaceInfo->descr,
					sizeof(usb_interface_descriptor)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.interface.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR_ETC:
		{
			uint32 endpointIndex = 0;
			const usb_interface_info *interfaceInfo = NULL;
			status = B_OK;
			if (op == B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR) {
				if (length < sizeof(command.endpoint))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.endpoint.config_index,
					command.endpoint.interface_index,
					B_USB_RAW_ACTIVE_ALTERNATE,
					&command.endpoint.status);
				endpointIndex = command.endpoint.endpoint_index;
			} else {
				if (length < sizeof(command.endpoint_etc))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.endpoint_etc.config_index,
					command.endpoint_etc.interface_index,
					command.endpoint_etc.alternate_index,
					&command.endpoint_etc.status);
				endpointIndex = command.endpoint_etc.endpoint_index;
			}

			if (interfaceInfo == NULL)
				break;

			if (endpointIndex >= interfaceInfo->endpoint_count) {
				command.endpoint.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			if (!IS_USER_ADDRESS(command.endpoint.descriptor)
				|| user_memcpy(command.endpoint.descriptor,
					interfaceInfo->endpoint[endpointIndex].descr,
					sizeof(usb_endpoint_descriptor)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			command.endpoint.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_GENERIC_DESCRIPTOR:
		case B_USB_RAW_COMMAND_GET_GENERIC_DESCRIPTOR_ETC:
		{
			uint32 genericIndex = 0;
			size_t genericLength = 0;
			const usb_interface_info *interfaceInfo = NULL;
			status = B_OK;
			if (op == B_USB_RAW_COMMAND_GET_GENERIC_DESCRIPTOR) {
				if (length < sizeof(command.generic))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.generic.config_index,
					command.generic.interface_index,
					B_USB_RAW_ACTIVE_ALTERNATE,
					&command.generic.status);
				genericIndex = command.generic.generic_index;
				genericLength = command.generic.length;
			} else {
				if (length < sizeof(command.generic_etc))
					return B_BUFFER_OVERFLOW;

				interfaceInfo = usb_raw_get_interface(device,
					command.generic_etc.config_index,
					command.generic_etc.interface_index,
					command.generic_etc.alternate_index,
					&command.generic_etc.status);
				genericIndex = command.generic_etc.generic_index;
				genericLength = command.generic_etc.length;
			}

			if (interfaceInfo == NULL)
				break;

			if (genericIndex >= interfaceInfo->generic_count) {
				command.endpoint.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			usb_descriptor *descriptor = interfaceInfo->generic[genericIndex];
			if (descriptor == NULL)
				break;

			if (!IS_USER_ADDRESS(command.generic.descriptor)
				|| user_memcpy(command.generic.descriptor, descriptor,
					min_c(genericLength, descriptor->generic.length)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			if (descriptor->generic.length > genericLength)
				command.generic.status = B_USB_RAW_STATUS_NO_MEMORY;
			else
				command.generic.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_GET_STRING_DESCRIPTOR:
		{
			if (length < sizeof(command.string))
				return B_BUFFER_OVERFLOW;

			size_t actualLength = 0;
			uint8 temp[4];
			status = B_OK;

			// We need to fetch the default language code, first.
			if (gUSBModule->get_descriptor(device->device,
				USB_DESCRIPTOR_STRING, 0, 0,
				temp, 4, &actualLength) < B_OK
				|| actualLength != 4
				|| temp[1] != USB_DESCRIPTOR_STRING) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				break;
			}
			const uint16 langid = (temp[2] | (temp[3] << 8));

			// Now fetch the string length.
			if (gUSBModule->get_descriptor(device->device,
				USB_DESCRIPTOR_STRING, command.string.string_index, langid,
				temp, 2, &actualLength) < B_OK
				|| actualLength != 2
				|| temp[1] != USB_DESCRIPTOR_STRING) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				break;
			}

			uint8 stringLength = MIN(temp[0], command.string.length);
			char *string = (char *)malloc(stringLength);
			if (string == NULL) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				status = B_NO_MEMORY;
				break;
			}

			if (gUSBModule->get_descriptor(device->device,
				USB_DESCRIPTOR_STRING, command.string.string_index, langid,
				string, stringLength, &actualLength) < B_OK
				|| actualLength != stringLength) {
				command.string.status = B_USB_RAW_STATUS_ABORTED;
				command.string.length = 0;
				free(string);
				break;
			}

			if (!IS_USER_ADDRESS(command.string.descriptor)
				|| user_memcpy(command.string.descriptor, string,
					stringLength) != B_OK) {
				free(string);
				return B_BAD_ADDRESS;
			}

			command.string.status = B_USB_RAW_STATUS_SUCCESS;
			command.string.length = stringLength;
			free(string);
			break;
		}

		case B_USB_RAW_COMMAND_GET_DESCRIPTOR:
		{
			if (length < sizeof(command.descriptor))
				return B_BUFFER_OVERFLOW;

			size_t actualLength = 0;
			uint8 firstTwoBytes[2];
			status = B_OK;

			if (gUSBModule->get_descriptor(device->device,
				command.descriptor.type, command.descriptor.index,
				command.descriptor.language_id, firstTwoBytes, 2,
				&actualLength) < B_OK
				|| actualLength != 2
				|| firstTwoBytes[1] != command.descriptor.type) {
				command.descriptor.status = B_USB_RAW_STATUS_ABORTED;
				command.descriptor.length = 0;
				break;
			}

			uint8 descriptorLength = MIN(firstTwoBytes[0],
				command.descriptor.length);
			uint8 *descriptorBuffer = (uint8 *)malloc(descriptorLength);
			if (descriptorBuffer == NULL) {
				command.descriptor.status = B_USB_RAW_STATUS_ABORTED;
				command.descriptor.length = 0;
				status = B_NO_MEMORY;
				break;
			}

			if (gUSBModule->get_descriptor(device->device,
				command.descriptor.type, command.descriptor.index,
				command.descriptor.language_id, descriptorBuffer,
				descriptorLength, &actualLength) < B_OK
				|| actualLength != descriptorLength) {
				command.descriptor.status = B_USB_RAW_STATUS_ABORTED;
				command.descriptor.length = 0;
				free(descriptorBuffer);
				break;
			}

			if (!IS_USER_ADDRESS(command.descriptor.data)
				|| user_memcpy(command.descriptor.data, descriptorBuffer,
					descriptorLength) != B_OK) {
				free(descriptorBuffer);
				return B_BAD_ADDRESS;
			}

			command.descriptor.status = B_USB_RAW_STATUS_SUCCESS;
			command.descriptor.length = descriptorLength;
			free(descriptorBuffer);
			break;
		}

		case B_USB_RAW_COMMAND_SET_CONFIGURATION:
		{
			if (length < sizeof(command.config))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device, command.config.config_index,
					&command.config.status);
			if (configurationInfo == NULL)
				break;

			// its pipe is about to go away
			usb_raw_stream_stop(device);

			if (gUSBModule->set_configuration(device->device,
				configurationInfo) < B_OK) {
				command.config.status = B_USB_RAW_STATUS_FAILED;
				break;
			}

			command.config.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_SET_ALT_INTERFACE:
		{
			if (length < sizeof(command.alternate))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				usb_raw_get_configuration(device,
					command.alternate.config_index,
					&command.alternate.status);
			if (configurationInfo == NULL)
				break;

			if (command.alternate.interface_index
				>= configurationInfo->interface_count) {
				command.alternate.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			const usb_interface_list *interfaceList =
				&configurationInfo->interface[command.alternate.interface_index];
			if (command.alternate.alternate_info >= interfaceList->alt_count) {
				command.alternate.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			// its pipes are about to go away; the streams of the device's
			// other interfaces go on
			usb_raw_stream_stop_interface(device,
				command.alternate.interface_index);

			if (gUSBModule->set_alt_interface(device->device,
				&interfaceList->alt[command.alternate.alternate_info]) < B_OK) {
				command.alternate.status = B_USB_RAW_STATUS_FAILED;
				break;
			}

			command.alternate.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_CONTROL_TRANSFER:
		{
			if (length < sizeof(command.control))
				return B_BUFFER_OVERFLOW;

			// requests without a data stage may come without a buffer
			void *controlData = NULL;
			if (command.control.length > 0) {
				controlData = malloc(command.control.length);
				if (controlData == NULL)
					return B_NO_MEMORY;
			}
			MemoryDeleter dataDeleter(controlData);

			bool inTransfer = (command.control.request_type
				& USB_ENDPOINT_ADDR_DIR_IN) != 0;
			if (command.control.length > 0
				&& (!IS_USER_ADDRESS(command.control.data)
					|| (!inTransfer && user_memcpy(controlData,
						command.control.data, command.control.length)
							!= B_OK))) {
				return B_BAD_ADDRESS;
			}

			MutexLocker deviceLocker(device->lock);
			if (gUSBModule->queue_request(device->device,
				command.control.request_type, command.control.request,
				command.control.value, command.control.index,
				command.control.length, controlData,
				usb_raw_callback, device) < B_OK) {
				command.control.status = B_USB_RAW_STATUS_FAILED;
				command.control.length = 0;
				status = B_OK;
				break;
			}

			status = acquire_sem_etc(device->notify, 1, B_KILL_CAN_INTERRUPT, 0);
			if (status != B_OK) {
				gUSBModule->cancel_queued_requests(device->device);
				acquire_sem(device->notify);
			}

			command.control.status = device->status;
			command.control.length = device->actual_length;
			deviceLocker.Unlock();

			if (command.control.status == B_OK)
				status = B_OK;
			if (inTransfer && command.control.length > 0
				&& user_memcpy(command.control.data, controlData,
					command.control.length) != B_OK) {
				status = B_BAD_ADDRESS;
			}
			break;
		}

		case B_USB_RAW_COMMAND_INTERRUPT_TRANSFER:
		case B_USB_RAW_COMMAND_BULK_TRANSFER:
		case B_USB_RAW_COMMAND_ISOCHRONOUS_TRANSFER:
		{
			if (length < sizeof(command.transfer))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				gUSBModule->get_configuration(device->device);
			if (configurationInfo == NULL) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_CONFIGURATION;
				break;
			}

			if (command.transfer.interface >= configurationInfo->interface_count) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			const usb_interface_info *interfaceInfo =
				configurationInfo->interface[command.transfer.interface].active;
			if (interfaceInfo == NULL) {
				command.transfer.status = B_USB_RAW_STATUS_ABORTED;
				break;
			}

			if (command.transfer.endpoint >= interfaceInfo->endpoint_count) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			const usb_endpoint_info *endpointInfo =
				&interfaceInfo->endpoint[command.transfer.endpoint];
			if (!endpointInfo->handle) {
				command.transfer.status = B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			size_t descriptorsSize = 0;
			usb_iso_packet_descriptor *packetDescriptors = NULL;
			void *transferData = NULL;
			MemoryDeleter descriptorsDeleter, dataDeleter;

			bool inTransfer = (endpointInfo->descr->endpoint_address
				& USB_ENDPOINT_ADDR_DIR_IN) != 0;
			if (op == B_USB_RAW_COMMAND_ISOCHRONOUS_TRANSFER) {
				if (length < sizeof(command.isochronous))
					return B_BUFFER_OVERFLOW;

				descriptorsSize = sizeof(usb_iso_packet_descriptor)
					* command.isochronous.packet_count;
				packetDescriptors
					= (usb_iso_packet_descriptor *)malloc(descriptorsSize);
				if (packetDescriptors == NULL) {
					command.transfer.status = B_USB_RAW_STATUS_NO_MEMORY;
					command.transfer.length = 0;
					break;
				}
				descriptorsDeleter.SetTo(packetDescriptors);

				if (!IS_USER_ADDRESS(command.isochronous.data)
					|| !IS_USER_ADDRESS(command.isochronous.packet_descriptors)
					|| user_memcpy(packetDescriptors,
						command.isochronous.packet_descriptors,
						descriptorsSize) != B_OK) {
					return B_BAD_ADDRESS;
				}
			} else {
				transferData = malloc(command.transfer.length);
				if (transferData == NULL) {
					command.transfer.status = B_USB_RAW_STATUS_NO_MEMORY;
					command.transfer.length = 0;
					break;
				}
				dataDeleter.SetTo(transferData);

				if (!IS_USER_ADDRESS(command.transfer.data) || (!inTransfer
						&& user_memcpy(transferData, command.transfer.data,
							command.transfer.length) != B_OK)) {
					return B_BAD_ADDRESS;
				}
			}

			status_t status;
			MutexLocker deviceLocker(device->lock);
			if (op == B_USB_RAW_COMMAND_INTERRUPT_TRANSFER) {
				status = gUSBModule->queue_interrupt(endpointInfo->handle,
					transferData, command.transfer.length,
					usb_raw_callback, device);
			} else if (op == B_USB_RAW_COMMAND_BULK_TRANSFER) {
				status = gUSBModule->queue_bulk(endpointInfo->handle,
					transferData, command.transfer.length,
					usb_raw_callback, device);
			} else {
				status = gUSBModule->queue_isochronous(endpointInfo->handle,
					command.isochronous.data, command.isochronous.length,
					packetDescriptors, command.isochronous.packet_count, NULL,
					0, usb_raw_callback, device);
			}

			if (status < B_OK) {
				command.transfer.status = B_USB_RAW_STATUS_FAILED;
				command.transfer.length = 0;
				status = B_OK;
				break;
			}

			status = acquire_sem_etc(device->notify, 1, B_KILL_CAN_INTERRUPT, 0);
			if (status != B_OK) {
				gUSBModule->cancel_queued_transfers(endpointInfo->handle);
				acquire_sem(device->notify);
			}

			command.transfer.status = device->status;
			command.transfer.length = device->actual_length;
			deviceLocker.Unlock();

			if (command.transfer.status == B_OK)
				status = B_OK;
			if (op == B_USB_RAW_COMMAND_ISOCHRONOUS_TRANSFER) {
				if (user_memcpy(command.isochronous.packet_descriptors,
						packetDescriptors, descriptorsSize) != B_OK) {
					status = B_BAD_ADDRESS;
				}
			} else {
				if (inTransfer && user_memcpy(command.transfer.data,
					transferData, command.transfer.length) != B_OK) {
					status = B_BAD_ADDRESS;
				}
			}

			break;
		}

		case B_USB_RAW_COMMAND_ISOCHRONOUS_STREAM_START:
		{
			if (length < sizeof(command.stream_start))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			const usb_configuration_info *configurationInfo =
				gUSBModule->get_configuration(device->device);
			if (configurationInfo == NULL) {
				command.stream_start.status
					= B_USB_RAW_STATUS_INVALID_CONFIGURATION;
				break;
			}

			if (command.stream_start.interface
					>= configurationInfo->interface_count) {
				command.stream_start.status
					= B_USB_RAW_STATUS_INVALID_INTERFACE;
				break;
			}

			const usb_interface_info *interfaceInfo = configurationInfo
				->interface[command.stream_start.interface].active;
			if (interfaceInfo == NULL) {
				command.stream_start.status = B_USB_RAW_STATUS_ABORTED;
				break;
			}

			if (command.stream_start.endpoint
					>= interfaceInfo->endpoint_count) {
				command.stream_start.status
					= B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			const usb_endpoint_info *endpointInfo
				= &interfaceInfo->endpoint[command.stream_start.endpoint];
			if (!endpointInfo->handle
				|| (endpointInfo->descr->attributes
					& USB_ENDPOINT_ATTR_MASK) != USB_ENDPOINT_ATTR_ISOCHRONOUS
				|| (endpointInfo->descr->endpoint_address
					& USB_ENDPOINT_ADDR_DIR_IN) == 0) {
				command.stream_start.status
					= B_USB_RAW_STATUS_INVALID_ENDPOINT;
				break;
			}

			status_t result = usb_raw_stream_start(device,
				command.stream_start.interface,
				endpointInfo->handle, command.stream_start.packet_size,
				command.stream_start.packets_per_transfer,
				command.stream_start.transfer_count,
				command.stream_start.buffer_size);
			if (result == B_NO_MEMORY)
				command.stream_start.status = B_USB_RAW_STATUS_NO_MEMORY;
			else if (result != B_OK)
				command.stream_start.status = B_USB_RAW_STATUS_FAILED;
			else
				command.stream_start.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}

		case B_USB_RAW_COMMAND_ISOCHRONOUS_STREAM_READ:
		{
			if (length < sizeof(command.stream_read))
				return B_BUFFER_OVERFLOW;
			if (!IS_USER_ADDRESS(command.stream_read.data))
				return B_BAD_ADDRESS;

			status_t result = usb_raw_stream_read(device, 0,
				command.stream_read.data, &command.stream_read.length,
				command.stream_read.timeout);
			status = B_OK;
			if (result == B_OK)
				command.stream_read.status = B_USB_RAW_STATUS_SUCCESS;
			else if (result == B_TIMED_OUT || result == B_WOULD_BLOCK)
				command.stream_read.status = B_USB_RAW_STATUS_TIMEOUT;
			else if (result == B_INTERRUPTED)
				status = B_INTERRUPTED;
			else if (result == B_BAD_ADDRESS)
				return B_BAD_ADDRESS;
			else
				command.stream_read.status = B_USB_RAW_STATUS_ABORTED;
			break;
		}

		case B_USB_RAW_COMMAND_ISOCHRONOUS_STREAM_STOP:
		{
			usb_raw_stream_stop(device);
			command.version.status = B_USB_RAW_STATUS_SUCCESS;
			status = B_OK;
			break;
		}

		case B_USB_RAW_COMMAND_ISOCHRONOUS_STREAM_READ_ENDPOINT:
		{
			if (length < sizeof(command.stream_read_endpoint))
				return B_BUFFER_OVERFLOW;
			if (!IS_USER_ADDRESS(command.stream_read_endpoint.data))
				return B_BAD_ADDRESS;

			status = B_OK;
			usb_pipe pipe = usb_raw_stream_pipe(device,
				command.stream_read_endpoint.interface,
				command.stream_read_endpoint.endpoint);
			if (pipe == 0) {
				command.stream_read_endpoint.status
					= B_USB_RAW_STATUS_INVALID_ENDPOINT;
				command.stream_read_endpoint.length = 0;
				break;
			}

			status_t result = usb_raw_stream_read(device, pipe,
				command.stream_read_endpoint.data,
				&command.stream_read_endpoint.length,
				command.stream_read_endpoint.timeout);
			if (result == B_OK) {
				command.stream_read_endpoint.status
					= B_USB_RAW_STATUS_SUCCESS;
			} else if (result == B_TIMED_OUT || result == B_WOULD_BLOCK) {
				command.stream_read_endpoint.status
					= B_USB_RAW_STATUS_TIMEOUT;
			} else if (result == B_INTERRUPTED)
				status = B_INTERRUPTED;
			else if (result == B_BAD_ADDRESS)
				return B_BAD_ADDRESS;
			else {
				command.stream_read_endpoint.status
					= B_USB_RAW_STATUS_FAILED;
			}
			break;
		}

		case B_USB_RAW_COMMAND_ISOCHRONOUS_STREAM_STOP_ENDPOINT:
		{
			if (length < sizeof(command.stream_stop_endpoint))
				return B_BUFFER_OVERFLOW;

			status = B_OK;
			usb_pipe pipe = usb_raw_stream_pipe(device,
				command.stream_stop_endpoint.interface,
				command.stream_stop_endpoint.endpoint);
			mutex_lock(&device->stream_lock);
			int32 slot = pipe != 0 ? usb_raw_stream_slot(device, pipe) : -1;
			mutex_unlock(&device->stream_lock);
			if (slot >= 0)
				usb_raw_stream_stop_slot(device, slot);
			command.stream_stop_endpoint.status = B_USB_RAW_STATUS_SUCCESS;
			break;
		}
	}

	if (user_memcpy(buffer, &command, min_c(length, sizeof(command))) != B_OK)
		return B_BAD_ADDRESS;

	return status;
}


static status_t
usb_raw_read(void *cookie, off_t position, void *buffer, size_t *length)
{
	TRACE((DRIVER_NAME": read()\n"));
	*length = 0;
	return B_OK;
}


static status_t
usb_raw_write(void *cookie, off_t position, const void *buffer, size_t *length)
{
	TRACE((DRIVER_NAME": write()\n"));
	*length = 0;
	return B_OK;
}


//
//#pragma mark -
//


status_t
init_hardware()
{
	TRACE((DRIVER_NAME": init_hardware()\n"));
	return B_OK;
}


status_t
init_driver()
{
	TRACE((DRIVER_NAME": init_driver()\n"));
	static usb_notify_hooks notifyHooks = {
		&usb_raw_device_added,
		&usb_raw_device_removed
	};

	gDeviceList = NULL;
	gDeviceCount = 0;
	mutex_init(&gDeviceListLock, "usb_raw device list lock");

	TRACE((DRIVER_NAME": trying module %s\n", B_USB_MODULE_NAME));
	status_t result = get_module(B_USB_MODULE_NAME,
		(module_info **)&gUSBModule);
	if (result < B_OK) {
		TRACE((DRIVER_NAME": getting module failed 0x%08" B_PRIx32 "\n",
			result));
		mutex_destroy(&gDeviceListLock);
		return result;
	}

	gUSBModule->register_driver(DRIVER_NAME, NULL, 0, NULL);
	gUSBModule->install_notify(DRIVER_NAME, &notifyHooks);
	return B_OK;
}


void
uninit_driver()
{
	TRACE((DRIVER_NAME": uninit_driver()\n"));
	gUSBModule->uninstall_notify(DRIVER_NAME);
	mutex_lock(&gDeviceListLock);

	if (gDeviceNames) {
		for (int32 i = 1; gDeviceNames[i]; i++)
			free(gDeviceNames[i]);
		free(gDeviceNames);
		gDeviceNames = NULL;
	}

	mutex_destroy(&gDeviceListLock);
	put_module(B_USB_MODULE_NAME);
}


const char **
publish_devices()
{
	TRACE((DRIVER_NAME": publish_devices()\n"));
	if (gDeviceNames) {
		for (int32 i = 0; gDeviceNames[i]; i++)
			free(gDeviceNames[i]);
		free(gDeviceNames);
		gDeviceNames = NULL;
	}

	int32 index = 0;
	gDeviceNames = (char **)malloc(sizeof(char *) * (gDeviceCount + 2));
	if (gDeviceNames == NULL)
		return NULL;

	gDeviceNames[index++] = strdup(DEVICE_NAME);

	mutex_lock(&gDeviceListLock);
	raw_device *element = gDeviceList;
	while (element) {
		gDeviceNames[index++] = strdup(element->name);
		element = (raw_device *)element->link;
	}

	gDeviceNames[index++] = NULL;
	mutex_unlock(&gDeviceListLock);
	return (const char **)gDeviceNames;
}


device_hooks *
find_device(const char *name)
{
	TRACE((DRIVER_NAME": find_device()\n"));
	static device_hooks hooks = {
		&usb_raw_open,
		&usb_raw_close,
		&usb_raw_free,
		&usb_raw_ioctl,
		&usb_raw_read,
		&usb_raw_write,
		NULL,
		NULL,
		NULL,
		NULL
	};

	return &hooks;
}
