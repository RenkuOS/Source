/*
 * Copyright 2003, Thomas Kurschel. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	DDC communication */


#include "ddc_int.h"
#include "ddc.h"
#include "i2c.h"

#include <KernelExport.h>
#include <OS.h>

#include <stdlib.h>


#define READ_RETRIES 4		// number of retries to read ddc data

#define TRACE_DDC
#ifdef TRACE_DDC
extern void _sPrintf(const char* format, ...);
#	define TRACE(x...) _sPrintf("DDC: " x)
#else
#	define TRACE(x...) ;
#endif


//! Verify checksum of DDC data.
static status_t
verify_checksum(const uint8 *data, size_t len)
{
	uint32 index;
	uint8 sum = 0;
	uint8 allOr = 0;

	for (index = 0; index < len; ++index, ++data) {
		sum += *data;
		allOr |= *data;
	}

	if (allOr == 0) {
		TRACE("%s: DDC information contains zeros only\n", __func__);
		return B_ERROR;
	}

	if (sum != 0) {
		TRACE("%s: Checksum error in DDC information\n", __func__);
		return B_IO_ERROR;
	}

	return B_OK;
}


static status_t
call_send_receive(const i2c_bus *bus, int slave_address,
	const uint8 *writeBuffer, size_t writeLength, uint8 *readBuffer,
	size_t readLength)
{
	i2c_send_receive send_receive = bus->send_receive;
	if (send_receive == NULL)
		send_receive = i2c_send_receive_callback;
	return send_receive(bus, slave_address, writeBuffer, writeLength, readBuffer,
		readLength);
}


//!	Read ddc2 data from monitor
static status_t
ddc2_read(const i2c_bus *bus, int start, uint8 *buffer, size_t length)
{
	status_t status = B_OK;
	uint8 writeBuffer[2];
	int i;

	writeBuffer[0] = start & 0xff;
	writeBuffer[1] = (start >> 8) & 0xff;

	for (i = 0; i < READ_RETRIES; ++i) {
		status = call_send_receive(bus, 0x50, writeBuffer,
			start < 0x100 ? 1 : 2, buffer, length);

		if (status != B_OK)
			TRACE("%s: DDC information read failure\n", __func__);

		if (status == B_OK) {
			status = verify_checksum(buffer, length);
			if (status == B_OK)
				break;

			dprintf("%s: DDC checksum incorrect!\n", __func__);
		}
	}

	return status;
}


/*!
	Reading VDIF has not been tested.
	it seems that almost noone supports VDIF which makes testing hard,
	but what's the point anyway?
*/
#if 0
static status_t
ddc2_read_vdif(const i2c_bus *bus, int start,
	void **vdif, size_t *vdif_len)
{
	status_t res;
	uint8 *data, *cur_data;
	int i;
	uint8 buffer[64];

	*vdif = NULL;
	*vdif_len = 0;

	res = ddc2_read(bus, start, buffer, 64);
	SHOW_INFO(2, "%x", buffer[0]);
	if (res != B_OK || buffer[0] == 0)
		return B_OK;

	// each block is 63 bytes plus 1 checksum long
	// we strip the checksum but store data directly into
	// buffer, so we need an extra byte for checksum of the last block
	data = malloc(buffer[0] * 63 + 1);
	if (data == NULL)
		return B_NO_MEMORY;

	cur_data = data;
	for (i = 0; i < buffer[0]; ++i) {
		ddc2_read(bus, start + i * 64, cur_data, 64);
		// strip checksum byte
		cur_data += 63;
	}

	*vdif_len = buffer[0] * 63;
	*vdif = data;
	return B_OK;
}
#endif


//	#pragma mark -


void
ddc2_init_timing(i2c_bus *bus)
{
	bus->send_receive = NULL;

	i2c_get100k_timing(&bus->timing);

	// VESA standard
	bus->timing.start_timeout = 550;
	bus->timing.byte_timeout = 2200;
	bus->timing.bit_timeout = 40;
	bus->timing.ack_start_timeout = 40;
	bus->timing.ack_timeout = 40;
}


//! Read EDID and VDIF from monitor via ddc2
status_t
ddc2_read_edid1(const i2c_bus *bus, edid1_info *edid,
	void **vdif, size_t *vdifLength)
{
	edid1_raw raw;
	status_t status = ddc2_read(bus, 0, (uint8 *)&raw, sizeof(raw));
	if (status != B_OK)
		return status;

	if (raw.version.version != 1 || raw.version.revision > 4) {
		TRACE("%s: EDID version or revision out of range\n", __func__);
		return B_ERROR;
	}

	edid_decode(edid, &raw);

	if (vdif != NULL)
		*vdif = NULL;
	if (vdifLength != NULL)
		*vdifLength = 0;

	// skip vdif as long as it's not tested	
#if 0	
	status = ddc2_read_vdif(bus, sizeof(raw) * (edid->num_sections + 1),
		vdif, vdifLength);
	if (status != B_OK)
		return status;
#endif

	return B_OK;
}


//	#pragma mark - DDC/CI (monitor control, VESA MCCS)


#define DDC_CI_ADDRESS			0x37
#define DDC_CI_HOST_ADDRESS		0x51
#define DDC_CI_REPLY_CHECKSUM	0x50
	// what the checksum of a monitor's reply starts with
#define DDC_CI_REPLY_DELAY		40000
	// microseconds the monitor is given to prepare its reply

#define DDC_CI_GET_VCP			0x01
#define DDC_CI_GET_VCP_REPLY	0x02
#define DDC_CI_SET_VCP			0x03


/*!	Sends a command to the monitor: the host's address, the length, the
	command, and a checksum over all of it and the monitor's address
*/
static status_t
ddc_ci_send(const i2c_bus *bus, const uint8 *command, uint8 length)
{
	uint8 buffer[8];
	uint8 checksum = DDC_CI_ADDRESS << 1;
	uint8 i;

	if (length > sizeof(buffer) - 3)
		return B_BAD_VALUE;

	buffer[0] = DDC_CI_HOST_ADDRESS;
	buffer[1] = 0x80 | length;
	for (i = 0; i < length; i++)
		buffer[i + 2] = command[i];
	for (i = 0; i < length + 2; i++)
		checksum ^= buffer[i];
	buffer[length + 2] = checksum;

	return call_send_receive(bus, DDC_CI_ADDRESS, buffer, length + 3, NULL, 0);
}


/*!	Reads the present and the maximum value of a VCP feature of the monitor.
	Request and reply are two transfers with a pause between them.
*/
status_t
ddc2_get_vcp_feature(const i2c_bus *bus, uint8 code, uint16 *_value,
	uint16 *_maximum)
{
	uint8 request[2];
	uint8 reply[11];
	uint8 checksum = DDC_CI_REPLY_CHECKSUM;
	size_t i;
	status_t status;

	request[0] = DDC_CI_GET_VCP;
	request[1] = code;
	status = ddc_ci_send(bus, request, sizeof(request));
	if (status != B_OK)
		return status;

	snooze(DDC_CI_REPLY_DELAY);

	status = call_send_receive(bus, DDC_CI_ADDRESS, NULL, 0, reply,
		sizeof(reply));
	if (status != B_OK)
		return status;

	// the monitor's address, the length, "VCP reply", result, VCP code, type,
	// maximum (2 bytes), present value (2 bytes), checksum
	for (i = 0; i < sizeof(reply) - 1; i++)
		checksum ^= reply[i];
	if (reply[1] != 0x88 || reply[2] != DDC_CI_GET_VCP_REPLY
		|| reply[4] != code || reply[10] != checksum) {
		TRACE("%s: no valid reply for VCP feature 0x%02x\n", __func__, code);
		return B_BAD_DATA;
	}
	if (reply[3] != 0)
		return B_UNSUPPORTED;

	*_maximum = (reply[6] << 8) | reply[7];
	*_value = (reply[8] << 8) | reply[9];
	return B_OK;
}


//!	Sets a VCP feature of the monitor
status_t
ddc2_set_vcp_feature(const i2c_bus *bus, uint8 code, uint16 value)
{
	uint8 command[4];

	command[0] = DDC_CI_SET_VCP;
	command[1] = code;
	command[2] = value >> 8;
	command[3] = value & 0xff;
	return ddc_ci_send(bus, command, sizeof(command));
}
