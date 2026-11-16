/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//! Hyper-V synthetic network adapter (netvsc): NVS + RNDIS over one VMBus channel


#include <new>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/if_media.h>

#include <condition_variable.h>
#include <device_manager.h>
#include <ether_driver.h>
#include <ethernet.h>
#include <hyperv.h>
#include <kernel.h>
#include <KernelExport.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>

#include "HyperVNetProtocol.h"


#define DRIVER_NAME		"hyperv_netvsc"
#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)
#define ERROR(x...)			dprintf(DRIVER_NAME ": " x)

#define HYPERV_NETVSC_DRIVER_MODULE_NAME	"drivers/network/hyperv_netvsc/driver_v1"
#define HYPERV_NETVSC_DEVICE_MODULE_NAME	"drivers/network/hyperv_netvsc/device_v1"
#define HYPERV_NETVSC_DEVICE_ID_GENERATOR	"hyperv_netvsc/device_id"

#define NETVSC_RING_SIZE		(128 * 1024)
#define NETVSC_RXBUF_SIZE		(2 * 1024 * 1024)
#define NETVSC_CHIM_SIZE		(1024 * 1024)
#define NETVSC_MAX_SECTIONS		512
#define NETVSC_RX_QUEUE_SIZE	256
#define NETVSC_PKT_BUFFER_SIZE	4096
#define NETVSC_TIMEOUT_US		5000000
#define NETVSC_FRAME_SIZE		ETHER_MAX_FRAME_SIZE

#define NETVSC_DEFAULT_FILTER	(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST \
	| NDIS_PACKET_TYPE_ALL_MULTICAST)

// VMBus transaction IDs: NVS requests, RNDIS control sends, TX send-buffer sections
#define TRANS_NVS			1
#define TRANS_CTRL			2
#define TRANS_TX_BASE		0x1000


static const uint32 kNVSVersions[] = {
	NVS_VERSION_61, NVS_VERSION_6, NVS_VERSION_5, NVS_VERSION_4, NVS_VERSION_2, NVS_VERSION_1
};


typedef struct {
	uint32	length;
	uint32	offset;
	uint64	page;
} _PACKED netvsc_gpa_range1;


typedef struct {
	device_node*				node;
	hyperv_device_interface*	hyperv;
	hyperv_device				hyperv_cookie;
	bool						channel_open;

	uint8*						packet;
	uint32						packet_length;

	uint32						nvs_version;
	ConditionVariable			nvs_event;
	uint8						nvs_response[64];
	uint32						nvs_response_length;

	uint8*						rxbuf;
	uint32						rxbuf_gpadl;
	bool						rxbuf_connected;
	uint8*						chim;
	uint32						chim_gpadl;
	bool						chim_connected;
	uint32						chim_section_size;
	uint32						chim_count;

	mutex						ctrl_lock;
	area_id						ctrl_area;
	void*						ctrl_page;
	phys_addr_t					ctrl_phys;
	uint32						ctrl_next_rid;
	int32						ctrl_pending_rid;
	ConditionVariable			ctrl_event;
	uint8						ctrl_response[256];
	uint32						ctrl_response_length;

	mutex						tx_lock;
	uint64						tx_bitmap[NETVSC_MAX_SECTIONS / 64];

	mutex						rx_lock;
	int32						open_count;
	sem_id						rx_sem;
	net_buffer*					rx_queue[NETVSC_RX_QUEUE_SIZE];
	uint32						rx_head;
	uint32						rx_count;
	bool						nonblocking;

	uint8						mac[ETHER_ADDRESS_LENGTH];
	bool						link_up;
	sem_id						link_sem;
} netvsc_cookie;


static device_manager_info* sDeviceManager;
static net_buffer_module_info* sBufferModule;


//	#pragma mark - channel receive path (DPC context)


static void
netvsc_handle_completion(netvsc_cookie* cookie, uint64 transactionID, const uint8* data,
	uint32 length)
{
	if (transactionID == TRANS_NVS) {
		cookie->nvs_response_length = min_c(length, sizeof(cookie->nvs_response));
		memcpy(cookie->nvs_response, data, cookie->nvs_response_length);
		cookie->nvs_event.NotifyAll();
	} else if (transactionID >= TRANS_TX_BASE
			&& transactionID < TRANS_TX_BASE + cookie->chim_count) {
		uint32 index = transactionID - TRANS_TX_BASE;
		MutexLocker locker(cookie->tx_lock);
		cookie->tx_bitmap[index / 64] &= ~(1ULL << (index % 64));
	}
	// TRANS_CTRL: the RNDIS completion itself arrives through the receive buffer
}


static void
netvsc_receive_frame(netvsc_cookie* cookie, const uint8* message, uint32 length)
{
	// The host shares the buffer: validate and use a copy of the header only
	rndis_packet_msg packet;
	if (length < sizeof(packet))
		return;
	memcpy(&packet, message, sizeof(packet));
	if (packet.length > length)
		return;

	uint32 offset = packet.data_offset + RNDIS_PACKET_OFFSET_BASE;
	if (packet.data_length < ETHER_HEADER_LENGTH || packet.data_length > NETVSC_FRAME_SIZE
			|| offset < sizeof(packet) || (uint64)offset + packet.data_length > packet.length)
		return;

	MutexLocker locker(cookie->rx_lock);
	if (cookie->rx_sem < 0 || cookie->rx_count == NETVSC_RX_QUEUE_SIZE)
		return;		// not open, or the stack is not keeping up: drop

	net_buffer* buffer = sBufferModule->create(0);
	if (buffer == NULL)
		return;
	if (sBufferModule->append(buffer, message + offset, packet.data_length) != B_OK) {
		sBufferModule->free(buffer);
		return;
	}

	cookie->rx_queue[(cookie->rx_head + cookie->rx_count) % NETVSC_RX_QUEUE_SIZE] = buffer;
	cookie->rx_count++;
	release_sem_etc(cookie->rx_sem, 1, B_DO_NOT_RESCHEDULE);
}


static void
netvsc_receive_rndis(netvsc_cookie* cookie, const uint8* message, uint32 length)
{
	if (length < sizeof(rndis_msg_header))
		return;

	uint32 type = reinterpret_cast<const rndis_msg_header*>(message)->type;
	if (type == REMOTE_NDIS_PACKET_MSG) {
		netvsc_receive_frame(cookie, message, length);
	} else if (type == REMOTE_NDIS_INDICATE_STATUS_MSG) {
		if (length < sizeof(rndis_status_msg))
			return;
		uint32 status = reinterpret_cast<const rndis_status_msg*>(message)->status;
		if (status != RNDIS_STATUS_MEDIA_CONNECT && status != RNDIS_STATUS_MEDIA_DISCONNECT)
			return;
		cookie->link_up = status == RNDIS_STATUS_MEDIA_CONNECT;
		TRACE_ALWAYS("link %s\n", cookie->link_up ? "up" : "down");
		if (cookie->link_sem >= 0)
			release_sem_etc(cookie->link_sem, 1, B_DO_NOT_RESCHEDULE);
	} else if ((type & REMOTE_NDIS_CMPLT_BIT) != 0 && length >= sizeof(rndis_comp_header)) {
		const rndis_comp_header* comp = reinterpret_cast<const rndis_comp_header*>(message);
		if (comp->request_id == 0
				|| (int32)comp->request_id != atomic_get(&cookie->ctrl_pending_rid))
			return;
		cookie->ctrl_response_length = min_c(length, sizeof(cookie->ctrl_response));
		memcpy(cookie->ctrl_response, message, cookie->ctrl_response_length);
		cookie->ctrl_event.NotifyAll();
	}
}


static void
netvsc_handle_rxbuf(netvsc_cookie* cookie, uint32 headerLength, const uint8* data,
	uint32 dataLength)
{
	const vmbus_pkt_transfer_header* xfer
		= reinterpret_cast<const vmbus_pkt_transfer_header*>(cookie->packet);

	if (dataLength >= sizeof(uint32)
			&& *reinterpret_cast<const uint32*>(data) == NVS_TYPE_RNDIS
			&& xfer->transfer_pageset_id == NVS_RXBUF_SIG
			&& headerLength >= sizeof(*xfer)
				+ (uint64)xfer->range_count * sizeof(vmbus_transfer_range)) {
		// Each range is one RNDIS message in the receive buffer
		for (uint32 i = 0; i < xfer->range_count; i++) {
			uint32 offset = xfer->ranges[i].offset;
			uint32 length = xfer->ranges[i].length;
			if ((uint64)offset + length > NETVSC_RXBUF_SIZE)
				continue;
			netvsc_receive_rndis(cookie, cookie->rxbuf + offset, length);
		}
	} else
		ERROR("invalid receive buffer packet\n");

	// Hand the ranges back to the host, or its receive buffer fills up
	nvs_msg_rndis_ack ack;
	memset(&ack, 0, sizeof(ack));
	ack.type = NVS_TYPE_RNDIS_ACK;
	ack.status = NVS_STATUS_OK;
	for (int retry = 0; cookie->channel_open; retry++) {
		status_t status = cookie->hyperv->write_packet(cookie->hyperv_cookie,
			VMBUS_PKTTYPE_COMPLETION, &ack, sizeof(ack), false, xfer->header.transaction_id);
		if (status == B_OK)
			return;
		if (status != B_DEV_NOT_READY)
			break;
		// The TX ring is full: wait for the host to drain it
		if (retry < 10)
			spin(100);
		else
			snooze(1000);
	}
	ERROR("receive buffer ack failed\n");
}


static void
netvsc_callback(void* data)
{
	netvsc_cookie* cookie = reinterpret_cast<netvsc_cookie*>(data);

	while (true) {
		uint32 length = cookie->packet_length;
		uint32 headerLength;
		uint32 dataLength;

		status_t status = cookie->hyperv->read_packet(cookie->hyperv_cookie, cookie->packet,
			&length, &headerLength, &dataLength);
		if (status == B_NO_MEMORY && length > cookie->packet_length) {
			uint8* packet = static_cast<uint8*>(realloc(cookie->packet, length));
			if (packet == NULL) {
				ERROR("cannot grow packet buffer to %" B_PRIu32 " bytes\n", length);
				break;
			}
			cookie->packet = packet;
			cookie->packet_length = length;
			continue;
		} else if (status == B_DEV_NOT_READY) {
			break;
		} else if (status != B_OK) {
			ERROR("failed to read packet (%s)\n", strerror(status));
			break;
		}

		const vmbus_pkt_header* header = reinterpret_cast<vmbus_pkt_header*>(cookie->packet);
		const uint8* payload = cookie->packet + headerLength;
		switch (header->type) {
			case VMBUS_PKTTYPE_COMPLETION:
				netvsc_handle_completion(cookie, header->transaction_id, payload, dataLength);
				break;
			case VMBUS_PKTTYPE_DATA_USING_TRANSFER_PAGES:
				netvsc_handle_rxbuf(cookie, headerLength, payload, dataLength);
				break;
			default:
				// In-band NVS notifications (TX table, VF association) are not used
				break;
		}
	}
}


//	#pragma mark - NVS and RNDIS requests


static status_t
netvsc_nvs_request(netvsc_cookie* cookie, const void* message, uint32 length,
	uint32 responseType, uint32 responseLength)
{
	cookie->nvs_response_length = 0;
	ConditionVariableEntry entry;
	cookie->nvs_event.Add(&entry);

	status_t status = cookie->hyperv->write_packet(cookie->hyperv_cookie,
		VMBUS_PKTTYPE_DATA_INBAND, message, length, true, TRANS_NVS);
	if (status != B_OK)
		return status;

	status = entry.Wait(B_RELATIVE_TIMEOUT, NETVSC_TIMEOUT_US);
	if (status != B_OK)
		return status;

	if (cookie->nvs_response_length < responseLength
			|| *reinterpret_cast<uint32*>(cookie->nvs_response) != responseType)
		return B_BAD_DATA;
	return B_OK;
}


static status_t
netvsc_nvs_send(netvsc_cookie* cookie, const void* message, uint32 length)
{
	return cookie->hyperv->write_packet(cookie->hyperv_cookie, VMBUS_PKTTYPE_DATA_INBAND,
		message, length, false, 0);
}


// Sends the RNDIS control message in ctrl_page and waits for its completion.
// Caller holds ctrl_lock.
static status_t
netvsc_rndis_execute(netvsc_cookie* cookie, uint32 length, uint32 completionType,
	uint32 completionLength)
{
	uint32 rid = reinterpret_cast<rndis_comp_header*>(cookie->ctrl_page)->request_id;
		// request_id is the third word of every RNDIS request
	atomic_set(&cookie->ctrl_pending_rid, (int32)rid);

	ConditionVariableEntry entry;
	cookie->ctrl_event.Add(&entry);

	netvsc_gpa_range1 range;
	range.length = length;
	range.offset = 0;
	range.page = cookie->ctrl_phys >> HV_PAGE_SHIFT;

	nvs_msg_rndis message;
	memset(&message, 0, sizeof(message));
	message.type = NVS_TYPE_RNDIS;
	message.rndis_mtype = NVS_RNDIS_MTYPE_CTRL;
	message.chim_idx = NVS_CHIM_IDX_INVALID;
	message.chim_sz = 0;

	status_t status = cookie->hyperv->write_gpa_packet(cookie->hyperv_cookie, 1,
		reinterpret_cast<vmbus_gpa_range*>(&range), sizeof(range), &message, sizeof(message),
		true, TRANS_CTRL);
	if (status == B_OK)
		status = entry.Wait(B_RELATIVE_TIMEOUT, NETVSC_TIMEOUT_US);
	atomic_set(&cookie->ctrl_pending_rid, 0);
	if (status != B_OK)
		return status;

	const rndis_comp_header* comp
		= reinterpret_cast<const rndis_comp_header*>(cookie->ctrl_response);
	if (cookie->ctrl_response_length < completionLength || comp->type != completionType)
		return B_BAD_DATA;
	if (comp->status != RNDIS_STATUS_SUCCESS) {
		ERROR("RNDIS request 0x%" B_PRIx32 " failed: status 0x%" B_PRIx32 "\n",
			completionType & ~REMOTE_NDIS_CMPLT_BIT, comp->status);
		return B_ERROR;
	}
	return B_OK;
}


static uint32
netvsc_rndis_next_rid(netvsc_cookie* cookie)
{
	if (++cookie->ctrl_next_rid == 0)
		cookie->ctrl_next_rid = 1;
	return cookie->ctrl_next_rid;
}


static status_t
netvsc_rndis_init(netvsc_cookie* cookie)
{
	MutexLocker locker(cookie->ctrl_lock);
	rndis_init_req* request = static_cast<rndis_init_req*>(cookie->ctrl_page);
	memset(request, 0, sizeof(*request));
	request->type = REMOTE_NDIS_INITIALIZE_MSG;
	request->length = sizeof(*request);
	request->request_id = netvsc_rndis_next_rid(cookie);
	request->ver_major = RNDIS_VERSION_MAJOR;
	request->ver_minor = RNDIS_VERSION_MINOR;
	request->max_xfer_size = RNDIS_XFER_SIZE;

	return netvsc_rndis_execute(cookie, sizeof(*request),
		REMOTE_NDIS_INITIALIZE_MSG | REMOTE_NDIS_CMPLT_BIT, sizeof(rndis_comp_header));
}


static status_t
netvsc_rndis_query(netvsc_cookie* cookie, uint32 oid, void* data, uint32 length)
{
	MutexLocker locker(cookie->ctrl_lock);
	rndis_oid_req* request = static_cast<rndis_oid_req*>(cookie->ctrl_page);
	memset(request, 0, sizeof(*request));
	request->type = REMOTE_NDIS_QUERY_MSG;
	request->length = sizeof(*request);
	request->request_id = netvsc_rndis_next_rid(cookie);
	request->oid = oid;
	// Not per the RNDIS spec (should be 0 without input), but Hyper-V fails the query otherwise
	request->info_offset = RNDIS_OID_REQ_INFO_OFFSET;

	status_t status = netvsc_rndis_execute(cookie, sizeof(*request),
		REMOTE_NDIS_QUERY_MSG | REMOTE_NDIS_CMPLT_BIT, sizeof(rndis_query_comp));
	if (status != B_OK)
		return status;

	const rndis_query_comp* comp
		= reinterpret_cast<const rndis_query_comp*>(cookie->ctrl_response);
	uint32 offset = comp->info_offset + RNDIS_PACKET_OFFSET_BASE;
	if (comp->info_length < length || offset < sizeof(*comp)
			|| (uint64)offset + length > cookie->ctrl_response_length)
		return B_BAD_DATA;

	memcpy(data, cookie->ctrl_response + offset, length);
	return B_OK;
}


static status_t
netvsc_rndis_set(netvsc_cookie* cookie, uint32 oid, uint32 value)
{
	MutexLocker locker(cookie->ctrl_lock);
	rndis_oid_req* request = static_cast<rndis_oid_req*>(cookie->ctrl_page);
	memset(request, 0, sizeof(*request));
	request->type = REMOTE_NDIS_SET_MSG;
	request->length = sizeof(*request) + sizeof(value);
	request->request_id = netvsc_rndis_next_rid(cookie);
	request->oid = oid;
	request->info_length = sizeof(value);
	request->info_offset = RNDIS_OID_REQ_INFO_OFFSET;
	memcpy(request + 1, &value, sizeof(value));

	return netvsc_rndis_execute(cookie, request->length,
		REMOTE_NDIS_SET_MSG | REMOTE_NDIS_CMPLT_BIT, sizeof(rndis_comp_header));
}


static status_t
netvsc_connect_buffer(netvsc_cookie* cookie, uint32 type, uint16 signature, uint32 size,
	uint8** _buffer, uint32* _gpadl, uint32 responseType, uint32 responseLength)
{
	status_t status = cookie->hyperv->allocate_gpadl(cookie->hyperv_cookie, size,
		(void**)_buffer, _gpadl);
	if (status != B_OK) {
		ERROR("failed to allocate %" B_PRIu32 " byte GPADL (%s)\n", size, strerror(status));
		return status;
	}

	nvs_msg_buf_conn message;
	memset(&message, 0, sizeof(message));
	message.type = type;
	message.gpadl = *_gpadl;
	message.sig = signature;

	status = netvsc_nvs_request(cookie, &message, sizeof(message), responseType, responseLength);
	if (status != B_OK)
		return status;

	// The status word follows the type in both connect responses
	if (reinterpret_cast<uint32*>(cookie->nvs_response)[1] != NVS_STATUS_OK)
		return B_ERROR;
	return B_OK;
}


static status_t
netvsc_connect(netvsc_cookie* cookie)
{
	status_t status = cookie->hyperv->open(cookie->hyperv_cookie, NETVSC_RING_SIZE,
		NETVSC_RING_SIZE, netvsc_callback, cookie);
	if (status != B_OK) {
		ERROR("failed to open channel (%s)\n", strerror(status));
		return status;
	}
	cookie->channel_open = true;

	// NVS version negotiation, newest first
	for (uint32 i = 0; i < B_COUNT_OF(kNVSVersions); i++) {
		nvs_msg_init init;
		memset(&init, 0, sizeof(init));
		init.type = NVS_TYPE_INIT;
		init.ver_min = init.ver_max = kNVSVersions[i];

		status = netvsc_nvs_request(cookie, &init, sizeof(init), NVS_TYPE_INIT_RESP,
			sizeof(nvs_msg_init_resp));
		if (status != B_OK)
			break;
		if (reinterpret_cast<nvs_msg_init_resp*>(cookie->nvs_response)->status
				== NVS_STATUS_OK) {
			cookie->nvs_version = kNVSVersions[i];
			break;
		}
	}
	if (cookie->nvs_version == 0) {
		ERROR("no NVS version accepted by the host\n");
		return status != B_OK ? status : B_NOT_SUPPORTED;
	}
	uint32 ndisVersion = cookie->nvs_version <= NVS_VERSION_4
		? NDIS_VERSION_6_1 : NDIS_VERSION_6_30;

	if (cookie->nvs_version >= NVS_VERSION_2) {
		nvs_msg_ndis_conf conf;
		memset(&conf, 0, sizeof(conf));
		conf.type = NVS_TYPE_NDIS_CONF;
		conf.mtu = NETVSC_FRAME_SIZE;
		status = netvsc_nvs_send(cookie, &conf, sizeof(conf));
		if (status != B_OK)
			return status;
	}

	nvs_msg_ndis_init ndisInit;
	memset(&ndisInit, 0, sizeof(ndisInit));
	ndisInit.type = NVS_TYPE_NDIS_INIT;
	ndisInit.ndis_major = ndisVersion >> 16;
	ndisInit.ndis_minor = ndisVersion & 0xffff;
	status = netvsc_nvs_send(cookie, &ndisInit, sizeof(ndisInit));
	if (status != B_OK)
		return status;

	status = netvsc_connect_buffer(cookie, NVS_TYPE_RXBUF_CONN, NVS_RXBUF_SIG, NETVSC_RXBUF_SIZE,
		&cookie->rxbuf, &cookie->rxbuf_gpadl, NVS_TYPE_RXBUF_CONNRESP,
		sizeof(nvs_msg_rxbuf_connresp));
	if (status != B_OK) {
		ERROR("receive buffer connect failed (%s)\n", strerror(status));
		return status;
	}
	cookie->rxbuf_connected = true;

	status = netvsc_connect_buffer(cookie, NVS_TYPE_CHIM_CONN, NVS_CHIM_SIG, NETVSC_CHIM_SIZE,
		&cookie->chim, &cookie->chim_gpadl, NVS_TYPE_CHIM_CONNRESP,
		sizeof(nvs_msg_chim_connresp));
	if (status != B_OK) {
		ERROR("send buffer connect failed (%s)\n", strerror(status));
		return status;
	}
	cookie->chim_connected = true;

	// shortcut: every frame is sent through a send buffer section, so a host
	// without usable sections is not supported; add GPA-direct sends if that happens.
	uint32 sectionSize = reinterpret_cast<nvs_msg_chim_connresp*>(cookie->nvs_response)->sectsz;
	if (sectionSize < sizeof(rndis_packet_msg) + NETVSC_FRAME_SIZE
			|| sectionSize > NETVSC_CHIM_SIZE || sectionSize % 4 != 0) {
		ERROR("unusable send buffer section size %" B_PRIu32 "\n", sectionSize);
		return B_NOT_SUPPORTED;
	}
	cookie->chim_section_size = sectionSize;
	cookie->chim_count = min_c(NETVSC_CHIM_SIZE / sectionSize, NETVSC_MAX_SECTIONS);

	// One page for RNDIS control messages, sent by guest physical address
	cookie->ctrl_area = create_area("hyperv_netvsc ctrl", &cookie->ctrl_page,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (cookie->ctrl_area < B_OK)
		return cookie->ctrl_area;
	physical_entry entry;
	status = get_memory_map(cookie->ctrl_page, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	cookie->ctrl_phys = entry.address;

	status = netvsc_rndis_init(cookie);
	if (status != B_OK) {
		ERROR("RNDIS initialize failed (%s)\n", strerror(status));
		return status;
	}

	status = netvsc_rndis_query(cookie, OID_802_3_PERMANENT_ADDRESS, cookie->mac,
		sizeof(cookie->mac));
	if (status != B_OK) {
		ERROR("MAC address query failed (%s)\n", strerror(status));
		return status;
	}

	uint32 mediaState;
	cookie->link_up = netvsc_rndis_query(cookie, OID_GEN_MEDIA_CONNECT_STATUS, &mediaState,
		sizeof(mediaState)) != B_OK || mediaState == NDIS_MEDIA_STATE_CONNECTED;

	TRACE_ALWAYS("NVS 0x%" B_PRIx32 ", NDIS %" B_PRIu32 ".%" B_PRIu32 ", %" B_PRIu32
		" send sections of %" B_PRIu32 " bytes, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
		cookie->nvs_version, ndisVersion >> 16, ndisVersion & 0xffff, cookie->chim_count,
		cookie->chim_section_size, cookie->mac[0], cookie->mac[1], cookie->mac[2],
		cookie->mac[3], cookie->mac[4], cookie->mac[5], cookie->link_up ? "up" : "down");
	return B_OK;
}


static void
netvsc_free_cookie(netvsc_cookie* cookie)
{
	if (cookie->channel_open) {
		if (cookie->ctrl_area >= B_OK) {
			// HALT has no completion
			MutexLocker locker(cookie->ctrl_lock);
			rndis_halt_req* halt = static_cast<rndis_halt_req*>(cookie->ctrl_page);
			halt->type = REMOTE_NDIS_HALT_MSG;
			halt->length = sizeof(*halt);
			halt->request_id = netvsc_rndis_next_rid(cookie);
			netvsc_gpa_range1 range = { sizeof(*halt), 0, cookie->ctrl_phys >> HV_PAGE_SHIFT };
			nvs_msg_rndis message;
			memset(&message, 0, sizeof(message));
			message.type = NVS_TYPE_RNDIS;
			message.rndis_mtype = NVS_RNDIS_MTYPE_CTRL;
			message.chim_idx = NVS_CHIM_IDX_INVALID;
			cookie->hyperv->write_gpa_packet(cookie->hyperv_cookie, 1,
				reinterpret_cast<vmbus_gpa_range*>(&range), sizeof(range), &message,
				sizeof(message), false, 0);
		}

		nvs_msg_buf_disconn disconn;
		memset(&disconn, 0, sizeof(disconn));
		if (cookie->rxbuf_connected) {
			disconn.type = NVS_TYPE_RXBUF_DISCONN;
			disconn.sig = NVS_RXBUF_SIG;
			netvsc_nvs_send(cookie, &disconn, sizeof(disconn));
		}
		if (cookie->chim_connected) {
			disconn.type = NVS_TYPE_CHIM_DISCONN;
			disconn.sig = NVS_CHIM_SIG;
			netvsc_nvs_send(cookie, &disconn, sizeof(disconn));
		}
		// Give the host time to let go of the buffers, as FreeBSD's hn(4) does
		snooze(200000);

		// Stops a receive buffer ack retrying in the callback
		cookie->channel_open = false;
		cookie->hyperv->close(cookie->hyperv_cookie);
	}
	if (cookie->rxbuf != NULL)
		cookie->hyperv->free_gpadl(cookie->hyperv_cookie, cookie->rxbuf_gpadl);
	if (cookie->chim != NULL)
		cookie->hyperv->free_gpadl(cookie->hyperv_cookie, cookie->chim_gpadl);
	if (cookie->ctrl_area >= B_OK)
		delete_area(cookie->ctrl_area);

	mutex_destroy(&cookie->ctrl_lock);
	mutex_destroy(&cookie->tx_lock);
	mutex_destroy(&cookie->rx_lock);
	free(cookie->packet);
	delete cookie;
}


//	#pragma mark - device module


static status_t
netvsc_send(netvsc_cookie* cookie, net_buffer* buffer)
{
	uint32 size = buffer->size;
	if (size > NETVSC_FRAME_SIZE)
		return B_BAD_VALUE;

	MutexLocker locker(cookie->tx_lock);
	uint32 index = 0;
	while (index < cookie->chim_count
			&& (cookie->tx_bitmap[index / 64] & (1ULL << (index % 64))) != 0)
		index++;
	if (index == cookie->chim_count)
		return B_WOULD_BLOCK;
			// shortcut: all sections in flight drops the frame; queue if this shows up
	cookie->tx_bitmap[index / 64] |= 1ULL << (index % 64);
	locker.Unlock();

	uint8* section = cookie->chim + index * cookie->chim_section_size;
	rndis_packet_msg* packet = reinterpret_cast<rndis_packet_msg*>(section);
	memset(packet, 0, sizeof(*packet));
	packet->type = REMOTE_NDIS_PACKET_MSG;
	packet->length = sizeof(*packet) + size;
	packet->data_offset = sizeof(*packet) - RNDIS_PACKET_OFFSET_BASE;
	packet->data_length = size;
	packet->pktinfo_offset = sizeof(*packet) - RNDIS_PACKET_OFFSET_BASE;

	status_t status = sBufferModule->read(buffer, 0, section + sizeof(*packet), size);
	if (status == B_OK) {
		nvs_msg_rndis message;
		memset(&message, 0, sizeof(message));
		message.type = NVS_TYPE_RNDIS;
		message.rndis_mtype = NVS_RNDIS_MTYPE_DATA;
		message.chim_idx = index;
		message.chim_sz = packet->length;
		status = cookie->hyperv->write_packet(cookie->hyperv_cookie, VMBUS_PKTTYPE_DATA_INBAND,
			&message, sizeof(message), true, TRANS_TX_BASE + index);
	}
	if (status != B_OK) {
		locker.Lock();
		cookie->tx_bitmap[index / 64] &= ~(1ULL << (index % 64));
		return status;
	}

	sBufferModule->free(buffer);
	return B_OK;
}


static status_t
netvsc_receive(netvsc_cookie* cookie, net_buffer** _buffer)
{
	MutexLocker locker(cookie->rx_lock);
	sem_id sem = cookie->rx_sem;
	locker.Unlock();

	// One semaphore count per queued buffer; a 0 relative timeout fails with B_WOULD_BLOCK
	status_t status = acquire_sem_etc(sem, 1,
		B_CAN_INTERRUPT | (cookie->nonblocking ? B_RELATIVE_TIMEOUT : 0), 0);
	if (status != B_OK)
		return status;

	locker.Lock();
	if (cookie->rx_count == 0)
		return B_INTERRUPTED;	// closed meanwhile
	*_buffer = cookie->rx_queue[cookie->rx_head];
	cookie->rx_head = (cookie->rx_head + 1) % NETVSC_RX_QUEUE_SIZE;
	cookie->rx_count--;
	return B_OK;
}


static status_t
netvsc_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
netvsc_uninit_device(void* deviceCookie)
{
}


static status_t netvsc_close(void* _cookie);


static status_t
netvsc_open(void* deviceCookie, const char* path, int openMode, void** _cookie)
{
	netvsc_cookie* cookie = reinterpret_cast<netvsc_cookie*>(deviceCookie);

	MutexLocker locker(cookie->rx_lock);
	if (cookie->open_count++ == 0) {
		cookie->rx_sem = create_sem(0, "hyperv_netvsc rx");
		if (cookie->rx_sem < B_OK) {
			status_t status = cookie->rx_sem;
			cookie->open_count--;
			return status;
		}
		cookie->nonblocking = (openMode & O_NONBLOCK) != 0;
		locker.Unlock();

		status_t status = netvsc_rndis_set(cookie, OID_GEN_CURRENT_PACKET_FILTER,
			NETVSC_DEFAULT_FILTER);
		if (status != B_OK) {
			ERROR("setting the receive filter failed (%s)\n", strerror(status));
			netvsc_close(cookie);
			return status;
		}
	}

	*_cookie = cookie;
	return B_OK;
}


static status_t
netvsc_close(void* _cookie)
{
	netvsc_cookie* cookie = reinterpret_cast<netvsc_cookie*>(_cookie);

	MutexLocker locker(cookie->rx_lock);
	if (--cookie->open_count > 0)
		return B_OK;

	// Wakes up a blocked receive
	delete_sem(cookie->rx_sem);
	cookie->rx_sem = -1;
	while (cookie->rx_count > 0) {
		sBufferModule->free(cookie->rx_queue[cookie->rx_head]);
		cookie->rx_head = (cookie->rx_head + 1) % NETVSC_RX_QUEUE_SIZE;
		cookie->rx_count--;
	}
	locker.Unlock();

	netvsc_rndis_set(cookie, OID_GEN_CURRENT_PACKET_FILTER, NDIS_PACKET_TYPE_NONE);
	return B_OK;
}


static status_t
netvsc_free(void* cookie)
{
	return B_OK;
}


static status_t
netvsc_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	netvsc_cookie* cookie = reinterpret_cast<netvsc_cookie*>(_cookie);

	switch (op) {
		case ETHER_INIT:
			return B_OK;

		case ETHER_GETADDR:
			return user_memcpy(buffer, cookie->mac, sizeof(cookie->mac));

		case ETHER_GETFRAMESIZE:
		{
			uint32 frameSize = NETVSC_FRAME_SIZE;
			if (length != sizeof(frameSize))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_NONBLOCK:
		case ETHER_SETPROMISC:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			if (op == ETHER_NONBLOCK) {
				cookie->nonblocking = value != 0;
				return B_OK;
			}
			return netvsc_rndis_set(cookie, OID_GEN_CURRENT_PACKET_FILTER,
				value != 0 ? NDIS_PACKET_TYPE_PROMISCUOUS : NETVSC_DEFAULT_FILTER);
		}

		// All multicast is always received
		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
			if (user_memcpy(&cookie->link_sem, buffer, sizeof(sem_id)) != B_OK) {
				cookie->link_sem = -1;
				return B_BAD_ADDRESS;
			}
			return B_OK;

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = (cookie->link_up ? IFM_ACTIVE : 0) | IFM_ETHER | IFM_FULL_DUPLEX
				| IFM_10G_T;
			state.speed = 10000000000ULL;
			state.quality = 1000;
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return netvsc_send(cookie, reinterpret_cast<net_buffer*>(buffer));

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return netvsc_receive(cookie, reinterpret_cast<net_buffer**>(buffer));
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module


static float
netvsc_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK)
		return -1;
	if (strcmp(bus, HYPERV_BUS_NAME) != 0)
		return 0.0f;

	const char* type;
	if (sDeviceManager->get_attr_string(parent, HYPERV_DEVICE_TYPE_STRING_ITEM, &type, false)
			!= B_OK || strcmp(type, VMBUS_TYPE_NETWORK) != 0)
		return 0.0f;

	return 0.8f;
}


static status_t
netvsc_register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = HYPERV_PRETTYNAME_NETWORK }},
		{ NULL }
	};

	return sDeviceManager->register_node(parent, HYPERV_NETVSC_DRIVER_MODULE_NAME, attributes,
		NULL, NULL);
}


static status_t
netvsc_init_driver(device_node* node, void** _driverCookie)
{
	netvsc_cookie* cookie = new(std::nothrow) netvsc_cookie();
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->node = node;
	cookie->ctrl_area = -1;
	cookie->rx_sem = -1;
	cookie->link_sem = -1;
	cookie->nvs_event.Init(cookie, "hyperv_netvsc nvs");
	cookie->ctrl_event.Init(cookie, "hyperv_netvsc rndis");
	mutex_init(&cookie->ctrl_lock, "hyperv_netvsc ctrl");
	mutex_init(&cookie->tx_lock, "hyperv_netvsc tx");
	mutex_init(&cookie->rx_lock, "hyperv_netvsc rx");
	cookie->packet_length = NETVSC_PKT_BUFFER_SIZE;
	cookie->packet = static_cast<uint8*>(malloc(cookie->packet_length));
	if (cookie->packet == NULL) {
		netvsc_free_cookie(cookie);
		return B_NO_MEMORY;
	}

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&cookie->hyperv,
		(void**)&cookie->hyperv_cookie);
	sDeviceManager->put_node(parent);

	status_t status = netvsc_connect(cookie);
	if (status != B_OK) {
		netvsc_free_cookie(cookie);
		return status;
	}

	*_driverCookie = cookie;
	return B_OK;
}


static void
netvsc_uninit_driver(void* driverCookie)
{
	netvsc_free_cookie(reinterpret_cast<netvsc_cookie*>(driverCookie));
}


static status_t
netvsc_register_child_devices(void* driverCookie)
{
	netvsc_cookie* cookie = reinterpret_cast<netvsc_cookie*>(driverCookie);

	int32 id = sDeviceManager->create_id(HYPERV_NETVSC_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/hyperv/%" B_PRId32, id);
	return sDeviceManager->publish_device(cookie->node, name, HYPERV_NETVSC_DEVICE_MODULE_NAME);
}


static device_module_info sHyperVNetDeviceModule = {
	{
		HYPERV_NETVSC_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	netvsc_init_device,
	netvsc_uninit_device,
	NULL,	// remove

	netvsc_open,
	netvsc_close,
	netvsc_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	netvsc_control,

	NULL,	// select
	NULL	// deselect
};


static driver_module_info sHyperVNetDriverModule = {
	{
		HYPERV_NETVSC_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	netvsc_supports_device,
	netvsc_register_device,
	netvsc_init_driver,
	netvsc_uninit_driver,
	netvsc_register_child_devices,
	NULL,	// rescan
	NULL	// removed
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule },
	{ NULL }
};


module_info* modules[] = {
	(module_info*)&sHyperVNetDriverModule,
	(module_info*)&sHyperVNetDeviceModule,
	NULL
};
