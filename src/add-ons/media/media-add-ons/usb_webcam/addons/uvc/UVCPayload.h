/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef UVC_PAYLOAD_H
#define UVC_PAYLOAD_H


#include <stddef.h>
#include <stdint.h>


//	Bits of the header's second byte. (UVC 1.0 § 2.4.3.3.)
static const uint8_t kUVCFrameIDBit = 0x01;
static const uint8_t kUVCEndOfFrameBit = 0x02;
static const uint8_t kUVCPresentationTimeBit = 0x04;
static const uint8_t kUVCSourceClockBit = 0x08;
static const uint8_t kUVCErrorBit = 0x40;

static const size_t kUVCMinHeaderLength = 2;
static const size_t kUVCPresentationTimeLength = 4;
static const size_t kUVCSourceTimeLength = 4;
static const size_t kUVCSourceClockLength = 6;

//	The SOF counter in the source clock counts USB frames (milliseconds) in
//	11 bits.
static const uint16_t kUVCFrameCounterMask = 0x7ff;
static const uint16_t kUVCFrameCounterHalf = 0x400;


struct uvc_payload_header {
			size_t				length;
			uint8_t				frameID;
			bool				endOfFrame;
			bool				error;
			bool				hasSOF;
			uint16_t			sof;
};


/*!	Reads the SOF counter of the source clock field, if the header has one
	that fits into its length.
*/
static inline void
uvc_parse_source_clock(const uint8_t* packet, uvc_payload_header* header)
{
	header->hasSOF = false;
	uint8_t flags = packet[1];
	if ((flags & kUVCSourceClockBit) == 0)
		return;

	size_t offset = kUVCMinHeaderLength;
	if ((flags & kUVCPresentationTimeBit) != 0)
		offset += kUVCPresentationTimeLength;
	if (header->length < offset + kUVCSourceClockLength)
		return;

	const uint8_t* counter = packet + offset + kUVCSourceTimeLength;
	header->sof = (counter[0] | (counter[1] << 8)) & kUVCFrameCounterMask;
	header->hasSOF = true;
}


/*!	Reads the header at the start of an isochronous packet. Returns false if
	the packet is too short to hold the header it announces; such a packet
	comes from the device and must not be trusted.
*/
static inline bool
uvc_parse_payload_header(const uint8_t* packet, size_t size,
	uvc_payload_header* header)
{
	if (size < kUVCMinHeaderLength)
		return false;

	size_t length = packet[0];
	if (length < kUVCMinHeaderLength || length > size)
		return false;

	uint8_t flags = packet[1];
	header->length = length;
	header->frameID = flags & kUVCFrameIDBit;
	header->endOfFrame = (flags & kUVCEndOfFrameBit) != 0;
	header->error = (flags & kUVCErrorBit) != 0;
	uvc_parse_source_clock(packet, header);
	return true;
}


/*!	Watches the SOF counter of the packets in the order they are handed over.
	The counter only goes forward; a step back means that the transfers came
	in swapped and the frame being assembled is mixed up.
*/
struct uvc_sof_order {
			bool				known;
			uint16_t			last;
};


static inline void
uvc_sof_order_reset(uvc_sof_order* order)
{
	order->known = false;
	order->last = 0;
}


/*!	Returns false if \a sof lies before the previous counter. Only the step
	back itself is reported: the counter is taken over either way, so a stream
	that starts anew is not held "behind".
*/
static inline bool
uvc_sof_in_order(uvc_sof_order* order, uint16_t sof)
{
	uint16_t step = (sof - order->last) & kUVCFrameCounterMask;
	bool inOrder = !order->known || step < kUVCFrameCounterHalf;
	order->known = true;
	order->last = sof;
	return inOrder;
}


/*!	A frame in which the camera reported an error is incomplete; showing it
	gives a torn or black picture.
*/
static inline bool
uvc_frame_usable(size_t length, bool hadError)
{
	return length > 0 && !hadError;
}


#endif	// UVC_PAYLOAD_H
