/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef XHCI_ISOCHRONOUS_H
#define XHCI_ISOCHRONOUS_H


#include <stddef.h>
#include <stdint.h>


/*!	Bits 0 to 10 of wMaxPacketSize are the packet size, bits 11 and 12 the
	number of additional transactions in the same microframe.
	(USB 2.0 § 9.6.6.)
*/
static inline uint32_t
xhci_isochronous_packet_size(uint32_t maxPacketSize)
{
	return maxPacketSize & 0x7ff;
}


/*!	Computes the two burst fields of an isochronous TRB that moves \a length
	bytes (XHCI 1.2 § 4.11.2.3): the number of bursts minus one, and the
	number of packets in the last burst minus one. \a burstPackets is the
	number of packets of one burst.
*/
static inline void
xhci_isochronous_burst(size_t length, uint32_t packetSize,
	uint32_t burstPackets, uint32_t* burstCount, uint32_t* lastBurstPackets)
{
	*burstCount = 0;
	*lastBurstPackets = 0;
	if (packetSize == 0 || burstPackets == 0)
		return;

	size_t packets = (length + packetSize - 1) / packetSize;
	if (packets == 0)
		packets = 1;

	size_t bursts = (packets + burstPackets - 1) / burstPackets;
	*burstCount = (uint32_t)(bursts - 1);
	*lastBurstPackets = (uint32_t)(packets - (bursts - 1) * burstPackets - 1);
}


#endif	// XHCI_ISOCHRONOUS_H
