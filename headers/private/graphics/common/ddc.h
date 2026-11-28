/*
 * Copyright (c) 2003, Thomas Kurschel
 * Distributed under the terms of the MIT License.
*/
#ifndef _DDC_H
#define _DDC_H


#include "i2c.h"
#include "edid.h"


#ifdef __cplusplus
extern "C" {
#endif

void ddc2_init_timing(i2c_bus *bus);

// read EDID and VDIF from monitor via ddc2
// (currently, *vdif and *vdif_len is always set to null)
status_t ddc2_read_edid1(const i2c_bus *bus, edid1_info *edid, 
	void **vdif, size_t *vdifLength);

// DDC/CI: read and set a VCP feature of the monitor (VESA MCCS).
// The monitor needs 50 ms between two commands; the caller sees to that.
#define DDC_VCP_LUMINANCE	0x10

status_t ddc2_get_vcp_feature(const i2c_bus *bus, uint8 code, uint16 *_value,
	uint16 *_maximum);
status_t ddc2_set_vcp_feature(const i2c_bus *bus, uint8 code, uint16 value);

#ifdef __cplusplus
}
#endif

#endif	/* _DDC_H */
