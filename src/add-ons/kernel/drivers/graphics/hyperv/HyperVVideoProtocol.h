/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _HYPERV_VIDEO_PROTOCOL_H_
#define _HYPERV_VIDEO_PROTOCOL_H_


#include <hyperv_spec.h>


#define HV_VID_RING_SIZE				0x10000
#define HV_VID_RX_PKT_BUFFER_SIZE		1024
#define HV_VID_TIMEOUT_US				HV_MS_TO_US(5000)

// Protocol versions keep the minor number in the upper 16 bits
#define HV_VID_VERSION(major, minor)	(((minor) << 16) | (major))
#define HV_VID_VERSION_WIN8				HV_VID_VERSION(3, 2)
#define HV_VID_VERSION_WIN10			HV_VID_VERSION(3, 5)

#define HV_VID_MAX_RESOLUTIONS			64
#define HV_VID_DEPTH					32

// Every message is wrapped in a pipe header of this type
#define HV_VID_PIPE_MSG_DATA			1

#define HV_VID_POINTER_SHAPE_COMPLETE	0xff


enum {
	HV_VID_MSGTYPE_ERROR				= 0,
	HV_VID_MSGTYPE_VERSION_REQUEST		= 1,
	HV_VID_MSGTYPE_VERSION_RESPONSE		= 2,
	HV_VID_MSGTYPE_VRAM_LOCATION		= 3,
	HV_VID_MSGTYPE_VRAM_LOCATION_ACK	= 4,
	HV_VID_MSGTYPE_SITUATION_UPDATE		= 5,
	HV_VID_MSGTYPE_SITUATION_UPDATE_ACK	= 6,
	HV_VID_MSGTYPE_POINTER_POSITION		= 7,
	HV_VID_MSGTYPE_POINTER_SHAPE		= 8,
	HV_VID_MSGTYPE_FEATURE_CHANGE		= 9,
	HV_VID_MSGTYPE_DIRT					= 10,
	HV_VID_MSGTYPE_RESOLUTION_REQUEST	= 13,
	HV_VID_MSGTYPE_RESOLUTION_RESPONSE	= 14
};


// Pipe header (type, bytes that follow it) and video header (type, bytes from its
// own start to the end of the message)
typedef struct {
	uint32	pipe_type;
	uint32	pipe_size;
	uint32	type;
	uint32	size;
} _PACKED hv_vid_msg_header;


typedef struct {
	hv_vid_msg_header	header;

	uint32	version;
} _PACKED hv_vid_msg_version_request;


typedef struct {
	hv_vid_msg_header	header;

	uint32	version;
	uint8	accepted;
	uint8	max_video_outputs;
} _PACKED hv_vid_msg_version_response;


// Version 3.5 and later: list of resolutions the host accepts
typedef struct {
	hv_vid_msg_header	header;

	uint8	max_resolutions;
} _PACKED hv_vid_msg_resolution_request;


typedef struct {
	uint16	width;
	uint16	height;
} _PACKED hv_vid_resolution;


typedef struct {
	hv_vid_msg_header	header;

	uint8				edid[128];
	uint8				resolution_count;
	uint8				default_resolution;
	uint8				is_standard;
	hv_vid_resolution	resolutions[HV_VID_MAX_RESOLUTIONS];
		// only resolution_count entries are sent
} _PACKED hv_vid_msg_resolution_response;


// Guest physical address the host should scan out from; the host echoes context
typedef struct {
	hv_vid_msg_header	header;

	uint64	context;
	uint8	address_valid;
	uint64	address;
} _PACKED hv_vid_msg_vram_location;


typedef struct {
	hv_vid_msg_header	header;

	uint64	context;
} _PACKED hv_vid_msg_vram_location_ack;


// Mode of the single video output
typedef struct {
	hv_vid_msg_header	header;

	uint64	context;
	uint8	output_count;
	uint8	active;
	uint32	vram_offset;
	uint8	depth;
	uint32	width;
	uint32	height;
	uint32	pitch;
} _PACKED hv_vid_msg_situation_update;


typedef struct {
	hv_vid_msg_header	header;

	uint8	visible;
	uint8	video_output;
	int32	x;
	int32	y;
} _PACKED hv_vid_msg_pointer_position;


typedef struct {
	hv_vid_msg_header	header;

	uint8	part;
	uint8	is_argb;
	uint32	width;
	uint32	height;
	uint32	hot_x;
	uint32	hot_y;
	uint8	data[4];
} _PACKED hv_vid_msg_pointer_shape;


// Sent by the host to say which updates it wants from the guest
typedef struct {
	hv_vid_msg_header	header;

	uint8	dirt_needed;
	uint8	pointer_position_needed;
	uint8	pointer_shape_needed;
	uint8	situation_needed;
} _PACKED hv_vid_msg_feature_change;


// Changed screen area; right and bottom are exclusive
typedef struct {
	hv_vid_msg_header	header;

	uint8	video_output;
	uint8	rect_count;
	int32	left;
	int32	top;
	int32	right;
	int32	bottom;
} _PACKED hv_vid_msg_dirt;


#endif // _HYPERV_VIDEO_PROTOCOL_H_
