/*
 * Copyright 2026, Kevin Adams, kevinadams05@gmail.com. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *		Kevin Adams, kevinadams05@gmail.com
 */


#include "hdmi.h"

#include <Debug.h>
#include <string.h>

#include "accelerant.h"
#include "accelerant_protos.h"
#include "encoder.h"


#undef TRACE

#define TRACE_HDMI
#ifdef TRACE_HDMI
#   define TRACE(x...) _sPrintf("radeon_hd: " x)
#else
#   define TRACE(x...) ;
#endif

#define ERROR(x...) _sPrintf("radeon_hd: " x)


// AVI InfoFrame version 2 header, HDMI 1.4a table 8-1 (p. 135)
#define AVI_INFOFRAME_TYPE		0x82
#define AVI_INFOFRAME_VERSION	0x02
#define AVI_INFOFRAME_LENGTH	13


// One AFMT block per DIG encoder, at the DIG block's register offset
static const uint32 kAfmtOffsets[] = {
	EVERGREEN_AFMT0_OFFSET,
	EVERGREEN_AFMT1_OFFSET,
	EVERGREEN_AFMT2_OFFSET,
	EVERGREEN_AFMT3_OFFSET,
	EVERGREEN_AFMT4_OFFSET,
	EVERGREEN_AFMT5_OFFSET,
	EVERGREEN_AFMT6_OFFSET,
};


struct cta_format {
	uint16	width;
	uint16	height;
	uint16	totalWidth;
	uint16	totalHeight;
	uint32	pixelClock;
	uint8	vic;
	uint8	pictureAspect;
};


// Video codes from HDMI 1.3a table 8-3 (p. 115). The timings are from
// VESA DMT 1.0 rev 13 (DMT IDs 04h, 55h, and 52h), which gives 720p and
// 1080p as the CEA-861 code 4 and 16 timings. Picture aspect: 1 = 4:3,
// 2 = 16:9.
static const cta_format kCtaFormats[] = {
	{ 640, 480, 800, 525, 25175, 1, 1 },
	{ 1280, 720, 1650, 750, 74250, 4, 2 },
	{ 1920, 1080, 2200, 1125, 148500, 16, 2 },
};


/*! Returns the CTA-861 format that matches the mode, or NULL. The
	59.94 Hz variants run at the pixel clock divided by 1.001, so allow
	1% either way.
*/
static const cta_format*
hdmi_match_cta_format(const display_timing& timing)
{
	if ((timing.flags & B_TIMING_INTERLACED) != 0)
		return NULL;

	for (uint32 i = 0; i < B_COUNT_OF(kCtaFormats); i++) {
		const cta_format& format = kCtaFormats[i];
		if (timing.h_display != format.width
			|| timing.v_display != format.height
			|| timing.h_total != format.totalWidth
			|| timing.v_total != format.totalHeight)
			continue;

		uint32 tolerance = format.pixelClock / 100;
		if (timing.pixel_clock + tolerance >= format.pixelClock
			&& timing.pixel_clock <= format.pixelClock + tolerance)
			return &format;
	}

	return NULL;
}


/*! Builds the AVI InfoFrame payload, PB0 to PB13, following HDMI 1.4a
	table 8-2 (p. 135): RGB, no bar or scan data, default quantization.
	A mode that isn't a CTA-861 format gets VIC 0, as HDMI 1.4a section
	8.2.1 (p. 136) requires.
*/
static void
hdmi_build_avi_infoframe(const display_mode& mode, uint8 buffer[14])
{
	memset(buffer, 0, 14);

	const cta_format* format = hdmi_match_cta_format(mode.timing);

	// PB1: Y = 0 (RGB), A0 = 1 (R0-R3 valid), B = 0, S = 0
	buffer[1] = 1 << 4;

	// PB2: C = 0, M = picture aspect, R = 8 (same as picture)
	buffer[2] = (format != NULL ? format->pictureAspect << 4 : 0) | 0x8;

	// PB4: video code
	buffer[4] = format != NULL ? format->vic : 0;

	// PB0: checksum, so that the header and payload sum to 0
	uint8 sum = AVI_INFOFRAME_TYPE + AVI_INFOFRAME_VERSION
		+ AVI_INFOFRAME_LENGTH;
	for (int i = 1; i <= AVI_INFOFRAME_LENGTH; i++)
		sum += buffer[i];
	buffer[0] = 0x100 - sum;
}


static uint32
hdmi_pack(const uint8* bytes)
{
	return (uint32)bytes[0] | (uint32)bytes[1] << 8 | (uint32)bytes[2] << 16
		| (uint32)bytes[3] << 24;
}


static bool
hdmi_afmt_offset(uint8 crtcID, uint32* offset, uint32* digID)
{
	radeon_shared_info &info = *gInfo->shared_info;

	// Only DCE 4 and newer have the AFMT block
	if (info.chipsetID < RADEON_CEDAR)
		return false;

	// DCE 4.1 has two blocks, DCE 4 to 6 have six, and DCE 8 has seven
	uint32 count = 6;
	if (info.dceMajor == 4 && info.dceMinor == 1)
		count = 2;
	else if (info.dceMajor >= 8)
		count = 7;

	// The AFMT block belongs to the connector's DIG encoder, not the CRTC
	*digID = encoder_pick_dig(gDisplay[crtcID]->connectorIndex);
	if (*digID >= count)
		return false;

	*offset = kAfmtOffsets[*digID];
	return true;
}


void
hdmi_avi_infoframe_program(uint8 crtcID, display_mode* mode)
{
	uint32 offset;
	uint32 digID;
	if (!hdmi_afmt_offset(crtcID, &offset, &digID)) {
		ERROR("%s: no AFMT block for CRTC %" B_PRIu8 "\n", __func__, crtcID);
		return;
	}

	uint8 buffer[14];
	hdmi_build_avi_infoframe(*mode, buffer);

	// Data islands must always carry valid packets, so keep the null and
	// General Control packets going. The other generators stay off.
	Write32(OUT, EVERGREEN_HDMI_VBI_PACKET_CONTROL + offset,
		EVERGREEN_HDMI_NULL_SEND | EVERGREEN_HDMI_GC_SEND
			| EVERGREEN_HDMI_GC_CONT);
	Write32(OUT, EVERGREEN_HDMI_ACR_PACKET_CONTROL + offset, 0);
	Write32(OUT, EVERGREEN_HDMI_GENERIC_PACKET_CONTROL + offset, 0);
	Write32(OUT, EVERGREEN_HDMI_AUDIO_PACKET_CONTROL + offset, 0);
	Write32(OUT, EVERGREEN_AFMT_AUDIO_PACKET_CONTROL + offset, 0);

	// The General Control packet carries AVMUTE, which blanks the sink
	uint32 gc = Read32(OUT, EVERGREEN_HDMI_GC + offset);
	Write32(OUT, EVERGREEN_HDMI_GC + offset, gc & ~EVERGREEN_HDMI_GC_AVMUTE);

	// 24-bit color only
	uint32 control = Read32(OUT, EVERGREEN_HDMI_CONTROL + offset);
	Write32(OUT, EVERGREEN_HDMI_CONTROL + offset,
		control & ~EVERGREEN_HDMI_DEEP_COLOR_ENABLE);

	// INFO0 holds the checksum and PB1 to PB3, INFO3 PB12, PB13, and the
	// version in its top byte
	Write32(OUT, EVERGREEN_AFMT_AVI_INFO0 + offset, hdmi_pack(buffer + 0));
	Write32(OUT, EVERGREEN_AFMT_AVI_INFO1 + offset, hdmi_pack(buffer + 4));
	Write32(OUT, EVERGREEN_AFMT_AVI_INFO2 + offset, hdmi_pack(buffer + 8));
	Write32(OUT, EVERGREEN_AFMT_AVI_INFO3 + offset, (uint32)buffer[12]
		| (uint32)buffer[13] << 8 | (uint32)AVI_INFOFRAME_VERSION << 24);

	// Send it on line 2 of every frame. Line 0 doesn't send.
	uint32 control1 = Read32(OUT, EVERGREEN_HDMI_INFOFRAME_CONTROL1 + offset);
	control1 &= ~EVERGREEN_HDMI_AVI_INFO_LINE_MASK;
	control1 |= EVERGREEN_HDMI_AVI_INFO_LINE(2);
	Write32(OUT, EVERGREEN_HDMI_INFOFRAME_CONTROL1 + offset, control1);

	Write32(OUT, EVERGREEN_HDMI_INFOFRAME_CONTROL0 + offset,
		EVERGREEN_HDMI_AVI_INFO_SEND | EVERGREEN_HDMI_AVI_INFO_CONT);

	TRACE("%s: CRTC %" B_PRIu8 ", DIG %" B_PRIu32 ": VIC %" B_PRIu8
		", aspect %" B_PRIu8 ", %" B_PRIu16 "x%" B_PRIu16 " at %" B_PRIu32
		" kHz\n", __func__, crtcID, digID, buffer[4], (buffer[2] >> 4) & 0x3,
		mode->timing.h_display, mode->timing.v_display,
		mode->timing.pixel_clock);
}


void
hdmi_registers_dump(uint8 crtcID)
{
	uint32 offset;
	uint32 digID;
	if (!hdmi_afmt_offset(crtcID, &offset, &digID))
		return;

	TRACE("%s: CRTC %" B_PRIu8 ", DIG %" B_PRIu32 ", AFMT offset 0x%"
		B_PRIx32 "\n", __func__, crtcID, digID, offset);
	TRACE("  HDMI_CONTROL            0x%08" B_PRIx32 "\n",
		Read32(OUT, EVERGREEN_HDMI_CONTROL + offset));
	TRACE("  HDMI_VBI_PACKET_CONTROL 0x%08" B_PRIx32 "\n",
		Read32(OUT, EVERGREEN_HDMI_VBI_PACKET_CONTROL + offset));
	TRACE("  HDMI_INFOFRAME_CONTROL  0x%08" B_PRIx32 " 0x%08" B_PRIx32 "\n",
		Read32(OUT, EVERGREEN_HDMI_INFOFRAME_CONTROL0 + offset),
		Read32(OUT, EVERGREEN_HDMI_INFOFRAME_CONTROL1 + offset));
	TRACE("  HDMI_GC                 0x%08" B_PRIx32 "\n",
		Read32(OUT, EVERGREEN_HDMI_GC + offset));
	TRACE("  AFMT_AVI_INFO0-3        0x%08" B_PRIx32 " 0x%08" B_PRIx32
		" 0x%08" B_PRIx32 " 0x%08" B_PRIx32 "\n",
		Read32(OUT, EVERGREEN_AFMT_AVI_INFO0 + offset),
		Read32(OUT, EVERGREEN_AFMT_AVI_INFO1 + offset),
		Read32(OUT, EVERGREEN_AFMT_AVI_INFO2 + offset),
		Read32(OUT, EVERGREEN_AFMT_AVI_INFO3 + offset));
}
