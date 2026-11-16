/*
 * Copyright 2005-2009, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef VIRTIO_INFO_H
#define VIRTIO_INFO_H


#include <Drivers.h>
#include <Accelerant.h>

#include <edid.h>


struct virtio_gpu_shared_info {
	area_id			mode_list_area;		// area containing display mode list
	uint32			mode_count;
	display_mode	current_mode;
	uint32			bytes_per_row;

	area_id			frame_buffer_area;	// area of frame buffer
	uint8*			frame_buffer;
		// pointer to frame buffer (visible by all apps!)

	edid1_raw		edid_raw;
	bool			has_edid;
	uint32			dpms_capabilities;

	char			name[32];
	uint32			vram_size;

	uint32			preferred_width;	// host's display size, clamped to
	uint32			preferred_height;	// the framebuffer
	sem_id			display_change_sem;	// released when it changes
};

//----------------- ioctl() interface ----------------

// list ioctls
enum {
	VIRTIO_GPU_GET_PRIVATE_DATA = B_DEVICE_OP_CODES_END + 1,
	VIRTIO_GPU_GET_DEVICE_NAME,
	VIRTIO_GPU_SET_DISPLAY_MODE,
	VIRTIO_GPU_SET_CURSOR,		// virtio_gpu_cursor_image
	VIRTIO_GPU_MOVE_CURSOR,		// virtio_gpu_cursor_position
	VIRTIO_GPU_SHOW_CURSOR,		// bool
};

#define VIRTIO_GPU_CURSOR_SIZE	64

struct virtio_gpu_cursor_image {
	uint16			hot_x;
	uint16			hot_y;
	uint32			bits[VIRTIO_GPU_CURSOR_SIZE * VIRTIO_GPU_CURSOR_SIZE];
		// B_RGBA32, unused pixels transparent
};

struct virtio_gpu_cursor_position {
	uint16			x;					// hot spot position
	uint16			y;
};


#endif	/* VIRTIO_INFO_H */
