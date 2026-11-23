/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _HYPERV_KBD_PROTOCOL_H_
#define _HYPERV_KBD_PROTOCOL_H_


#include <hyperv_spec.h>


#define HV_KBD_RING_SIZE				0x8000
#define HV_KBD_RX_PKT_BUFFER_SIZE		256
#define HV_KBD_REQUEST_TRANS_ID			0xCAFECAFE
#define HV_KBD_TIMEOUT_US				HV_MS_TO_US(5000)

// Keyboard protocol version 1.0
#define HV_KBD_VERSION_V1_0				((1 << 16) | 0)


// Keyboard message types
enum {
	HV_KBD_MSGTYPE_PROTOCOL_REQUEST		= 1,
	HV_KBD_MSGTYPE_PROTOCOL_RESPONSE	= 2,
	HV_KBD_MSGTYPE_EVENT				= 3,
	HV_KBD_MSGTYPE_LED_INDICATORS		= 4
};


// Protocol response status
#define HV_KBD_PROTOCOL_ACCEPTED		0x1

// Keystroke info flags
#define HV_KBD_INFO_UNICODE				0x1
#define HV_KBD_INFO_BREAK				0x2
#define HV_KBD_INFO_E0					0x4
#define HV_KBD_INFO_E1					0x8


typedef struct {
	uint32	type;
} _PACKED hv_kbd_msg_header;


// Protocol request message sent to Hyper-V
typedef struct {
	hv_kbd_msg_header	header;

	uint32	version;
} _PACKED hv_kbd_msg_protocol_request;


// Protocol response message received from Hyper-V
typedef struct {
	hv_kbd_msg_header	header;

	uint32	status;
} _PACKED hv_kbd_msg_protocol_response;


// Keystroke event received from Hyper-V, make_code is a scan code set 1 make code
typedef struct {
	hv_kbd_msg_header	header;

	uint16	make_code;
	uint16	reserved;
	uint32	info;
} _PACKED hv_kbd_msg_keystroke;


typedef union {
	hv_kbd_msg_header				header;
	hv_kbd_msg_protocol_response	protocol_resp;
	hv_kbd_msg_keystroke			keystroke;
} _PACKED hv_kbd_in_msg;


#endif // _HYPERV_KBD_PROTOCOL_H_
