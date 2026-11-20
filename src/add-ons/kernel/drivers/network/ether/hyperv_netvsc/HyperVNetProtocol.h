/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * The protocol definitions follow FreeBSD's hn(4) (sys/dev/hyperv/netvsc/if_hnreg.h,
 * sys/net/rndis.h), Copyright (c) 2009-2017 Microsoft Corp., BSD-2-Clause.
 */
#ifndef _HYPERV_NET_PROTOCOL_H_
#define _HYPERV_NET_PROTOCOL_H_


#include <SupportDefs.h>


// NVS (Network Virtualization Service) versions, newest first in the driver
#define NVS_VERSION_1				0x00002
#define NVS_VERSION_2				0x30002
#define NVS_VERSION_4				0x40000
#define NVS_VERSION_5				0x50000
#define NVS_VERSION_6				0x60000
#define NVS_VERSION_61				0x60001

#define NDIS_VERSION_6_1			0x00060001
#define NDIS_VERSION_6_30			0x0006001e

#define NVS_RXBUF_SIG				0xcafe
#define NVS_CHIM_SIG				0xface
#define NVS_CHIM_IDX_INVALID		0xffffffff

#define NVS_RNDIS_MTYPE_DATA		0
#define NVS_RNDIS_MTYPE_CTRL		1

#define NVS_STATUS_OK				1

#define NVS_TYPE_INIT				1
#define NVS_TYPE_INIT_RESP			2
#define NVS_TYPE_NDIS_INIT			100
#define NVS_TYPE_RXBUF_CONN			101
#define NVS_TYPE_RXBUF_CONNRESP		102
#define NVS_TYPE_RXBUF_DISCONN		103
#define NVS_TYPE_CHIM_CONN			104
#define NVS_TYPE_CHIM_CONNRESP		105
#define NVS_TYPE_CHIM_DISCONN		106
#define NVS_TYPE_RNDIS				107
#define NVS_TYPE_RNDIS_ACK			108
#define NVS_TYPE_NDIS_CONF			125


// Every NVS request is padded to 40 bytes; the host ignores shorter ones.
typedef struct {
	uint32	type;
	uint32	ver_min;
	uint32	ver_max;
	uint8	reserved[28];
} _PACKED nvs_msg_init;

typedef struct {
	uint32	type;
	uint32	ver;
	uint32	reserved;
	uint32	status;
} _PACKED nvs_msg_init_resp;

typedef struct {
	uint32	type;
	uint32	mtu;
	uint32	reserved;
	uint64	caps;
	uint8	reserved1[20];
} _PACKED nvs_msg_ndis_conf;

typedef struct {
	uint32	type;
	uint32	ndis_major;
	uint32	ndis_minor;
	uint8	reserved[28];
} _PACKED nvs_msg_ndis_init;

// RXBUF_CONN and CHIM_CONN
typedef struct {
	uint32	type;
	uint32	gpadl;
	uint16	sig;
	uint8	reserved[30];
} _PACKED nvs_msg_buf_conn;

typedef struct {
	uint32	type;
	uint32	status;
	uint32	nsect;
	// followed by section descriptors, unused here
} _PACKED nvs_msg_rxbuf_connresp;

typedef struct {
	uint32	type;
	uint32	status;
	uint32	sectsz;
} _PACKED nvs_msg_chim_connresp;

// RXBUF_DISCONN and CHIM_DISCONN
typedef struct {
	uint32	type;
	uint16	sig;
	uint8	reserved[34];
} _PACKED nvs_msg_buf_disconn;

typedef struct {
	uint32	type;
	uint32	rndis_mtype;
	uint32	chim_idx;
	uint32	chim_sz;
	uint8	reserved[24];
} _PACKED nvs_msg_rndis;

typedef struct {
	uint32	type;
	uint32	status;
	uint8	reserved[32];
} _PACKED nvs_msg_rndis_ack;


// RNDIS
#define RNDIS_VERSION_MAJOR				1
#define RNDIS_VERSION_MINOR				0

#define RNDIS_STATUS_SUCCESS			0x00000000
#define RNDIS_STATUS_MEDIA_CONNECT		0x4001000B
#define RNDIS_STATUS_MEDIA_DISCONNECT	0x4001000C

#define REMOTE_NDIS_PACKET_MSG			0x00000001
#define REMOTE_NDIS_INITIALIZE_MSG		0x00000002
#define REMOTE_NDIS_HALT_MSG			0x00000003
#define REMOTE_NDIS_QUERY_MSG			0x00000004
#define REMOTE_NDIS_SET_MSG				0x00000005
#define REMOTE_NDIS_INDICATE_STATUS_MSG	0x00000007
#define REMOTE_NDIS_CMPLT_BIT			0x80000000

#define OID_GEN_CURRENT_PACKET_FILTER	0x0001010E
#define OID_GEN_MEDIA_CONNECT_STATUS	0x00010114
#define OID_802_3_PERMANENT_ADDRESS		0x01010101

#define NDIS_MEDIA_STATE_CONNECTED		0

#define NDIS_PACKET_TYPE_NONE			0x00000000
#define NDIS_PACKET_TYPE_DIRECTED		0x00000001
#define NDIS_PACKET_TYPE_ALL_MULTICAST	0x00000004
#define NDIS_PACKET_TYPE_BROADCAST		0x00000008
#define NDIS_PACKET_TYPE_PROMISCUOUS	0x00000020

#define RNDIS_XFER_SIZE					2048


typedef struct {
	uint32	type;
	uint32	length;
} _PACKED rndis_msg_header;

typedef struct {
	uint32	type;
	uint32	length;
	uint32	data_offset;		// offsets are relative to data_offset itself
	uint32	data_length;
	uint32	oob_data_offset;
	uint32	oob_data_length;
	uint32	oob_data_elements;
	uint32	pktinfo_offset;
	uint32	pktinfo_length;
	uint32	vc_handle;
	uint32	reserved;
} _PACKED rndis_packet_msg;

#define RNDIS_PACKET_OFFSET_BASE	8	// offsetof(rndis_packet_msg, data_offset)

typedef struct {
	uint32	type;
	uint32	length;
	uint32	request_id;
	uint32	status;
} _PACKED rndis_comp_header;

typedef struct {
	uint32	type;
	uint32	length;
	uint32	request_id;
	uint32	ver_major;
	uint32	ver_minor;
	uint32	max_xfer_size;
} _PACKED rndis_init_req;

typedef struct {
	uint32	type;
	uint32	length;
	uint32	request_id;
} _PACKED rndis_halt_req;

// QUERY and SET
typedef struct {
	uint32	type;
	uint32	length;
	uint32	request_id;
	uint32	oid;
	uint32	info_length;
	uint32	info_offset;		// relative to request_id
	uint32	device_vc_handle;
} _PACKED rndis_oid_req;

#define RNDIS_OID_REQ_INFO_OFFSET	(sizeof(rndis_oid_req) - 8)

typedef struct {
	uint32	type;
	uint32	length;
	uint32	request_id;
	uint32	status;
	uint32	info_length;
	uint32	info_offset;		// relative to request_id
} _PACKED rndis_query_comp;

typedef struct {
	uint32	type;
	uint32	length;
	uint32	status;
	uint32	status_length;
	uint32	status_offset;
} _PACKED rndis_status_msg;


#endif // _HYPERV_NET_PROTOCOL_H_
