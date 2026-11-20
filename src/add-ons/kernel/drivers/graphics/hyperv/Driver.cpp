/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Hyper-V synthetic video driver (VMBus synthvid protocol 3.5 and 3.2).

	The host scans out from the frame buffer the EFI firmware set up; the driver
	tells the host its location and the current mode, and reports changed areas.
	It implements the ioctl interface of the VESA driver, so app_server uses the
	VESA accelerant with the mode list below.
*/


#include <new>
#include <stdlib.h>
#include <string.h>

#include <boot_item.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <frame_buffer_console.h>
#include <graphic_driver.h>
#include <hyperv.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vesa_info.h>
#include <vm/vm.h>

#include "HyperVVideoProtocol.h"


#define DRIVER_NAME					"hyperv_video"
#define ACCELERANT_NAME				"vesa.accelerant"
#define HYPERV_VIDEO_DRIVER_MODULE_NAME	"drivers/graphics/hyperv_video/driver_v1"
#define HYPERV_VIDEO_DEVICE_MODULE_NAME	"drivers/graphics/hyperv_video/device_v1"
#define HYPERV_VIDEO_PUBLISH_PATH	"graphics/hyperv/0"

//#define TRACE_HYPERV_VIDEO
#ifdef TRACE_HYPERV_VIDEO
#	define TRACE(x...)	dprintf(DRIVER_NAME ": " x)
#else
#	define TRACE(x...)	;
#endif
#define ERROR(x...)		dprintf(DRIVER_NAME ": " x)

#define DIRT_INTERVAL_US	50000
	// the host only redraws areas reported dirty; 20 Hz full-screen updates


typedef struct {
	device_node*				node;
	hyperv_device_interface*	hyperv;
	hyperv_device				hyperv_cookie;
	bool						channel_open;
	uint8*						packet;

	ConditionVariable			response_event;
	uint32						response_type;
		// type of the response being waited for, 0 if none
	uint8						response[HV_VID_RX_PKT_BUFFER_SIZE];
	uint32						response_length;

	int32						dirt_needed;
	int32						hide_pointer;

	mutex						lock;
		// serializes mode changes against the dirt updates
	phys_addr_t					vram_physical;
	size_t						vram_size;
	area_id						vram_area;
	addr_t						vram;

	area_id						shared_area;
	vesa_shared_info*			shared_info;
	vesa_mode*					modes;

	thread_id					dirt_thread;
	bool						dirt_thread_running;
} video_driver_cookie;


// Offered to version 3.2 hosts, which cannot list their resolutions
static const hv_vid_resolution kWin8Resolutions[] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 },
	{ 1280, 800 }, { 1280, 1024 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }
};


static device_manager_info* sDeviceManager;


static status_t
hyperv_video_send(video_driver_cookie* cookie, void* message, uint32 length, uint32 type)
{
	hv_vid_msg_header* header = reinterpret_cast<hv_vid_msg_header*>(message);
	header->pipe_type = HV_VID_PIPE_MSG_DATA;
	header->pipe_size = length - offsetof(hv_vid_msg_header, type);
	header->type = type;
	header->size = header->pipe_size;

	return cookie->hyperv->write_packet(cookie->hyperv_cookie, VMBUS_PKTTYPE_DATA_INBAND,
		message, length, false, 0);
}


//! Sends a message and waits for the response of type \a responseType.
static status_t
hyperv_video_request(video_driver_cookie* cookie, void* message, uint32 length, uint32 type,
	uint32 responseType, uint32 minResponseLength)
{
	ConditionVariableEntry entry;
	cookie->response_event.Add(&entry);
	atomic_set((int32*)&cookie->response_type, responseType);

	status_t status = hyperv_video_send(cookie, message, length, type);
	if (status == B_OK)
		status = entry.Wait(B_RELATIVE_TIMEOUT, HV_VID_TIMEOUT_US);
	atomic_set((int32*)&cookie->response_type, 0);

	if (status != B_OK) {
		ERROR("No response to message %" B_PRIu32 " (%s)\n", type, strerror(status));
		return status;
	}
	if (cookie->response_length < minResponseLength) {
		ERROR("Response to message %" B_PRIu32 " too short (%" B_PRIu32 ")\n", type,
			cookie->response_length);
		return B_BAD_DATA;
	}
	return B_OK;
}


static status_t
hyperv_video_update_situation(video_driver_cookie* cookie, uint32 width, uint32 height)
{
	hv_vid_msg_situation_update message = {};
	message.output_count = 1;
	message.active = 1;
	message.vram_offset = 0;
	message.depth = HV_VID_DEPTH;
	message.width = width;
	message.height = height;
	message.pitch = width * (HV_VID_DEPTH / 8);
	return hyperv_video_send(cookie, &message, sizeof(message),
		HV_VID_MSGTYPE_SITUATION_UPDATE);
}


//! Hides the pointer the host otherwise draws on top of the screen.
static void
hyperv_video_hide_pointer(video_driver_cookie* cookie)
{
	hv_vid_msg_pointer_position position = {};
	position.visible = 1;
		// the host ignores a position with visible 0, so send a visible pointer
		// with a transparent 1x1 shape instead
	hyperv_video_send(cookie, &position, sizeof(position), HV_VID_MSGTYPE_POINTER_POSITION);

	hv_vid_msg_pointer_shape shape = {};
	shape.part = HV_VID_POINTER_SHAPE_COMPLETE;
	shape.is_argb = 1;
	shape.width = 1;
	shape.height = 1;
	hyperv_video_send(cookie, &shape, sizeof(shape), HV_VID_MSGTYPE_POINTER_SHAPE);
}


static void
hyperv_video_callback(void* data)
{
	video_driver_cookie* cookie = reinterpret_cast<video_driver_cookie*>(data);

	while (true) {
		uint32 length = HV_VID_RX_PKT_BUFFER_SIZE;
		uint32 headerLength;
		uint32 messageLength;

		status_t status = cookie->hyperv->read_packet(cookie->hyperv_cookie, cookie->packet,
			&length, &headerLength, &messageLength);
		if (status == B_DEV_NOT_READY) {
			break;
		} else if (status != B_OK) {
			ERROR("Failed to read packet (%s)\n", strerror(status));
			break;
		}

		if (messageLength < sizeof(hv_vid_msg_header))
			continue;

		hv_vid_msg_header* header = reinterpret_cast<hv_vid_msg_header*>(cookie->packet
			+ headerLength);
		if (header->pipe_type != HV_VID_PIPE_MSG_DATA)
			continue;

		if (header->type == HV_VID_MSGTYPE_FEATURE_CHANGE) {
			if (messageLength < sizeof(hv_vid_msg_feature_change))
				continue;
			hv_vid_msg_feature_change* change
				= reinterpret_cast<hv_vid_msg_feature_change*>(header);
			TRACE("Feature change: dirt %u pointer position %u shape %u situation %u\n",
				change->dirt_needed, change->pointer_position_needed,
				change->pointer_shape_needed, change->situation_needed);
			atomic_set(&cookie->dirt_needed, change->dirt_needed != 0);
			if (change->dirt_needed != 0) {
				// the host shows its pointer again after VM Connect reconnects
				atomic_set(&cookie->hide_pointer, 1);
			}
			continue;
		}

		if (header->type != 0 && header->type == (uint32)atomic_get(
				(int32*)&cookie->response_type)) {
			memcpy(cookie->response, header, messageLength);
			cookie->response_length = messageLength;
			atomic_set((int32*)&cookie->response_type, 0);
			cookie->response_event.NotifyAll();
			continue;
		}

		TRACE("Unexpected message type %" B_PRIu32 "\n", header->type);
	}
}


static status_t
hyperv_video_dirt_thread(void* data)
{
	video_driver_cookie* cookie = reinterpret_cast<video_driver_cookie*>(data);

	while (cookie->dirt_thread_running) {
		snooze(DIRT_INTERVAL_US);

		if (atomic_get_and_set(&cookie->hide_pointer, 0) != 0)
			hyperv_video_hide_pointer(cookie);
		if (atomic_get(&cookie->dirt_needed) == 0)
			continue;

		MutexLocker locker(cookie->lock);
		hv_vid_msg_dirt message = {};
		message.rect_count = 1;
		message.right = cookie->shared_info->current_mode.virtual_width;
		message.bottom = cookie->shared_info->current_mode.virtual_height;
		hyperv_video_send(cookie, &message, sizeof(message), HV_VID_MSGTYPE_DIRT);
	}
	return B_OK;
}


//! Adds a mode if it fits into the VRAM and is not listed yet.
static void
hyperv_video_add_mode(video_driver_cookie* cookie, uint32 width, uint32 height)
{
	if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX
		|| (uint64)width * height * (HV_VID_DEPTH / 8) > cookie->vram_size)
		return;

	vesa_shared_info* info = cookie->shared_info;
	for (uint32 i = 0; i < info->vesa_mode_count; i++) {
		if (cookie->modes[i].width == width && cookie->modes[i].height == height)
			return;
	}

	vesa_mode& mode = cookie->modes[info->vesa_mode_count];
	mode.mode = info->vesa_mode_count;
	mode.width = width;
	mode.height = height;
	mode.bits_per_pixel = HV_VID_DEPTH;
	info->vesa_mode_count++;
}


static status_t
hyperv_video_connect(video_driver_cookie* cookie, const frame_buffer_boot_info* bootInfo)
{
	status_t status = cookie->hyperv->open(cookie->hyperv_cookie, HV_VID_RING_SIZE,
		HV_VID_RING_SIZE, hyperv_video_callback, cookie);
	if (status != B_OK) {
		ERROR("Failed to open channel\n");
		return status;
	}
	cookie->channel_open = true;

	static const uint32 kVersions[] = { HV_VID_VERSION_WIN10, HV_VID_VERSION_WIN8 };
	uint32 version = 0;
	for (size_t i = 0; i < B_COUNT_OF(kVersions); i++) {
		hv_vid_msg_version_request request = {};
		request.version = kVersions[i];
		status = hyperv_video_request(cookie, &request, sizeof(request),
			HV_VID_MSGTYPE_VERSION_REQUEST, HV_VID_MSGTYPE_VERSION_RESPONSE,
			sizeof(hv_vid_msg_version_response));
		if (status != B_OK)
			return status;
		if (reinterpret_cast<hv_vid_msg_version_response*>(cookie->response)->accepted != 0) {
			version = kVersions[i];
			break;
		}
	}
	if (version == 0) {
		ERROR("Host rejected all protocol versions\n");
		return B_NOT_SUPPORTED;
	}
	TRACE("Protocol version %u.%u\n", version & 0xffff, version >> 16);

	// The boot mode comes first so it stays available
	hyperv_video_add_mode(cookie, bootInfo->width, bootInfo->height);

	if (version == HV_VID_VERSION_WIN10) {
		hv_vid_msg_resolution_request request = {};
		request.max_resolutions = HV_VID_MAX_RESOLUTIONS;
		status = hyperv_video_request(cookie, &request, sizeof(request),
			HV_VID_MSGTYPE_RESOLUTION_REQUEST, HV_VID_MSGTYPE_RESOLUTION_RESPONSE,
			offsetof(hv_vid_msg_resolution_response, resolutions));
		if (status != B_OK)
			return status;

		hv_vid_msg_resolution_response* response
			= reinterpret_cast<hv_vid_msg_resolution_response*>(cookie->response);
		uint32 count = min_c(response->resolution_count, HV_VID_MAX_RESOLUTIONS);
		count = min_c(count, (cookie->response_length
			- offsetof(hv_vid_msg_resolution_response, resolutions))
			/ sizeof(hv_vid_resolution));
		for (uint32 i = 0; i < count; i++) {
			TRACE("Host resolution %u x %u\n", response->resolutions[i].width,
				response->resolutions[i].height);
			hyperv_video_add_mode(cookie, response->resolutions[i].width,
				response->resolutions[i].height);
		}
	} else {
		for (size_t i = 0; i < B_COUNT_OF(kWin8Resolutions); i++)
			hyperv_video_add_mode(cookie, kWin8Resolutions[i].width, kWin8Resolutions[i].height);
	}
	TRACE("%" B_PRIu32 " modes fit into %" B_PRIuSIZE " bytes of VRAM\n",
		cookie->shared_info->vesa_mode_count, cookie->vram_size);

	hv_vid_msg_vram_location location = {};
	location.context = cookie->vram_physical;
	location.address_valid = 1;
	location.address = cookie->vram_physical;
	status = hyperv_video_request(cookie, &location, sizeof(location),
		HV_VID_MSGTYPE_VRAM_LOCATION, HV_VID_MSGTYPE_VRAM_LOCATION_ACK,
		sizeof(hv_vid_msg_vram_location_ack));
	if (status != B_OK)
		return status;
	if (reinterpret_cast<hv_vid_msg_vram_location_ack*>(cookie->response)->context
			!= cookie->vram_physical) {
		ERROR("Host did not accept the VRAM location\n");
		return B_ERROR;
	}
	TRACE("VRAM at %#" B_PRIxPHYSADDR " accepted\n", cookie->vram_physical);

	status = hyperv_video_update_situation(cookie, bootInfo->width, bootInfo->height);
	if (status != B_OK)
		return status;

	hyperv_video_hide_pointer(cookie);
	return B_OK;
}


static void
hyperv_video_free_cookie(video_driver_cookie* cookie)
{
	if (cookie->dirt_thread >= 0) {
		cookie->dirt_thread_running = false;
		status_t result;
		wait_for_thread(cookie->dirt_thread, &result);
	}
	if (cookie->channel_open)
		cookie->hyperv->close(cookie->hyperv_cookie);
	if (cookie->vram_area >= 0) {
		frame_buffer_update(0, 0, 0, 0, 0);
		vm_change_clones_to_null_areas(cookie->vram_area);
		delete_area(cookie->vram_area);
	}
	if (cookie->shared_area >= 0)
		delete_area(cookie->shared_area);
	mutex_destroy(&cookie->lock);
	free(cookie->packet);
	delete cookie;
}


//	#pragma mark - device module


static status_t
hyperv_video_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
hyperv_video_uninit_device(void* deviceCookie)
{
}


static status_t
hyperv_video_open(void* deviceCookie, const char* path, int openMode, void** _cookie)
{
	*_cookie = deviceCookie;
	return B_OK;
}


static status_t
hyperv_video_close(void* cookie)
{
	return B_OK;
}


static status_t
hyperv_video_free(void* cookie)
{
	return B_OK;
}


static status_t
hyperv_video_read(void* cookie, off_t pos, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
hyperv_video_write(void* cookie, off_t pos, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
hyperv_video_set_mode(video_driver_cookie* cookie, uint32 index)
{
	MutexLocker locker(cookie->lock);
	vesa_shared_info* info = cookie->shared_info;
	if (index >= info->vesa_mode_count)
		return B_BAD_VALUE;

	uint32 width = cookie->modes[index].width;
	uint32 height = cookie->modes[index].height;
	uint32 bytesPerRow = width * (HV_VID_DEPTH / 8);

	status_t status = hyperv_video_update_situation(cookie, width, height);
	if (status != B_OK)
		return status;

	info->current_mode.virtual_width = width;
	info->current_mode.virtual_height = height;
	info->current_mode.space = B_RGB32;
	info->bytes_per_row = bytesPerRow;
	frame_buffer_update(cookie->vram, width, height, HV_VID_DEPTH, bytesPerRow);

	TRACE("Mode %" B_PRIu32 " x %" B_PRIu32 " set\n", width, height);
	return B_OK;
}


static status_t
hyperv_video_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	video_driver_cookie* cookie = reinterpret_cast<video_driver_cookie*>(_cookie);

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			if (user_strlcpy((char*)buffer, ACCELERANT_NAME, B_FILE_NAME_LENGTH) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;

		case VESA_GET_PRIVATE_DATA:
			return user_memcpy(buffer, &cookie->shared_area, sizeof(area_id));

		case VESA_GET_DEVICE_NAME:
			if (user_strlcpy((char*)buffer, HYPERV_VIDEO_PUBLISH_PATH, B_PATH_NAME_LENGTH)
					< B_OK)
				return B_BAD_ADDRESS;
			return B_OK;

		case VESA_CLONE_FRAME_BUFFER:
		{
			void* address;
			area_id area = vm_clone_area(B_CURRENT_TEAM, "cloned hyperv_video vram",
				&address, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, REGION_NO_PRIVATE_MAP,
				cookie->vram_area, true);
			if (area < 0)
				return area;

			return _user_get_area_info(area, (area_info*)buffer);
		}

		case VESA_SET_DISPLAY_MODE:
		{
			uint32 index;
			if (length != sizeof(index))
				return B_BAD_VALUE;
			if (user_memcpy(&index, buffer, sizeof(index)) != B_OK)
				return B_BAD_ADDRESS;
			return hyperv_video_set_mode(cookie, index);
		}

		default:
			return B_DEV_INVALID_IOCTL;
	}
}


//	#pragma mark - driver module


static float
hyperv_video_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK)
		return -1;

	if (strcmp(bus, HYPERV_BUS_NAME) != 0)
		return 0.0f;

	const char* type;
	if (sDeviceManager->get_attr_string(parent, HYPERV_DEVICE_TYPE_STRING_ITEM, &type, false)
			!= B_OK)
		return 0.0f;

	if (strcmp(type, VMBUS_TYPE_DISPLAY) != 0)
		return 0.0f;

	return 0.8f;
}


static status_t
hyperv_video_register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = HYPERV_PRETTYNAME_DISPLAY }},
		{ NULL }
	};

	return sDeviceManager->register_node(parent, HYPERV_VIDEO_DRIVER_MODULE_NAME, attributes,
		NULL, NULL);
}


static status_t
hyperv_video_init_driver(device_node* node, void** _driverCookie)
{
	// The host scans out from the firmware frame buffer, so that is our VRAM
	const frame_buffer_boot_info* bootInfo
		= (const frame_buffer_boot_info*)get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
	if (bootInfo == NULL || bootInfo->depth != HV_VID_DEPTH
		|| bootInfo->bytes_per_row != bootInfo->width * (HV_VID_DEPTH / 8)
		|| (bootInfo->physical_frame_buffer & (B_PAGE_SIZE - 1)) != 0) {
		ERROR("No usable boot frame buffer\n");
		return B_NO_INIT;
	}

	video_driver_cookie* cookie = new(std::nothrow) video_driver_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->node = node;
	cookie->channel_open = false;
	cookie->response_type = 0;
	cookie->response_length = 0;
	cookie->dirt_needed = 0;
	cookie->hide_pointer = 0;
	cookie->vram_physical = bootInfo->physical_frame_buffer;
	cookie->vram_size = 0;
	cookie->vram_area = -1;
	cookie->shared_area = -1;
	cookie->dirt_thread = -1;
	cookie->dirt_thread_running = false;
	cookie->response_event.Init(cookie, "hyper-v video response");
	mutex_init(&cookie->lock, "hyper-v video lock");
	cookie->packet = static_cast<uint8*>(malloc(HV_VID_RX_PKT_BUFFER_SIZE));
	if (cookie->packet == NULL) {
		hyperv_video_free_cookie(cookie);
		return B_NO_MEMORY;
	}

	device_node* parent = sDeviceManager->get_parent_node(node);
	status_t status = sDeviceManager->get_driver(parent, (driver_module_info**)&cookie->hyperv,
		(void**)&cookie->hyperv_cookie);
	uint32 vramSize = 0;
	sDeviceManager->get_attr_uint32(parent, HYPERV_MMIO_SIZE_ITEM, &vramSize, false);
	sDeviceManager->put_node(parent);
	if (status != B_OK) {
		hyperv_video_free_cookie(cookie);
		return status;
	}

	// Assumes the firmware frame buffer starts at the device's MMIO range, which
	// bounds it: the MMIO size can be larger than the boot mode needs.
	cookie->vram_size = vramSize;
	if (cookie->vram_size < (size_t)bootInfo->bytes_per_row * bootInfo->height) {
		ERROR("VRAM size %" B_PRIuSIZE " too small\n", cookie->vram_size);
		hyperv_video_free_cookie(cookie);
		return B_NO_INIT;
	}

	// The host maps the VRAM cacheable; on arm64 the guest must match it
#ifdef __aarch64__
	const uint32 memoryType = B_WRITE_BACK_MEMORY;
#else
	const uint32 memoryType = B_WRITE_COMBINING_MEMORY;
#endif
	cookie->vram_area = map_physical_memory("hyperv_video vram", cookie->vram_physical,
		cookie->vram_size, B_ANY_KERNEL_ADDRESS | memoryType, B_READ_AREA | B_WRITE_AREA,
		(void**)&cookie->vram);

	size_t sharedSize = sizeof(vesa_shared_info)
		+ (HV_VID_MAX_RESOLUTIONS + 1) * sizeof(vesa_mode);
	cookie->shared_area = create_area("hyperv_video shared info",
		(void**)&cookie->shared_info, B_ANY_KERNEL_ADDRESS,
		(sharedSize + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (cookie->vram_area < 0 || cookie->shared_area < 0) {
		hyperv_video_free_cookie(cookie);
		return B_NO_MEMORY;
	}

	vesa_shared_info* info = cookie->shared_info;
	memset(info, 0, sharedSize);
	info->vesa_mode_offset = sizeof(vesa_shared_info);
	cookie->modes = (vesa_mode*)((uint8*)info + info->vesa_mode_offset);
	info->current_mode.virtual_width = bootInfo->width;
	info->current_mode.virtual_height = bootInfo->height;
	info->current_mode.space = B_RGB32;
	info->bytes_per_row = bootInfo->bytes_per_row;
	info->vram_size = cookie->vram_size;
	strlcpy(info->name, "Hyper-V", sizeof(info->name));

	status = hyperv_video_connect(cookie, bootInfo);
	if (status == B_OK) {
		cookie->dirt_thread_running = true;
		cookie->dirt_thread = spawn_kernel_thread(hyperv_video_dirt_thread,
			"hyper-v video dirt", B_NORMAL_PRIORITY, cookie);
		if (cookie->dirt_thread < 0)
			status = cookie->dirt_thread;
		else
			resume_thread(cookie->dirt_thread);
	}
	if (status != B_OK) {
		hyperv_video_free_cookie(cookie);
		return status;
	}

	*_driverCookie = cookie;
	return B_OK;
}


static void
hyperv_video_uninit_driver(void* driverCookie)
{
	hyperv_video_free_cookie(reinterpret_cast<video_driver_cookie*>(driverCookie));
}


static status_t
hyperv_video_register_child_devices(void* driverCookie)
{
	video_driver_cookie* cookie = reinterpret_cast<video_driver_cookie*>(driverCookie);
	return sDeviceManager->publish_device(cookie->node, HYPERV_VIDEO_PUBLISH_PATH,
		HYPERV_VIDEO_DEVICE_MODULE_NAME);
}


static device_module_info sHyperVVideoDeviceModule = {
	{
		HYPERV_VIDEO_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	hyperv_video_init_device,
	hyperv_video_uninit_device,
	NULL,	// remove

	hyperv_video_open,
	hyperv_video_close,
	hyperv_video_free,
	hyperv_video_read,
	hyperv_video_write,
	NULL,	// io
	hyperv_video_control,

	NULL,	// select
	NULL	// deselect
};


static driver_module_info sHyperVVideoDriverModule = {
	{
		HYPERV_VIDEO_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	hyperv_video_supports_device,
	hyperv_video_register_device,
	hyperv_video_init_driver,
	hyperv_video_uninit_driver,
	hyperv_video_register_child_devices,
	NULL,	// rescan
	NULL	// removed
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NULL }
};


module_info* modules[] = {
	(module_info*)&sHyperVVideoDriverModule,
	(module_info*)&sHyperVVideoDeviceModule,
	NULL
};
