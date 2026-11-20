/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "xhci_isochronous.h"

#include <stdio.h>


static int sFailures = 0;
static int sTests = 0;


#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			sFailures++; \
		} \
	} while (0)


//	The isochronous endpoint of the Lite-On camera (USB 04ca:7066) at its
//	largest setting: 960 bytes and two extra transactions per microframe.


static void
test_one_packet_is_one_burst_with_one_packet()
{
	uint32_t burstCount, lastBurstPackets;
	xhci_isochronous_burst(640, 640, 1, &burstCount, &lastBurstPackets);
	CHECK(burstCount == 0);
	CHECK(lastBurstPackets == 0);
}


static void
test_three_full_packets_are_one_burst_of_three()
{
	uint32_t burstCount, lastBurstPackets;
	xhci_isochronous_burst(2880, 960, 3, &burstCount, &lastBurstPackets);
	CHECK(burstCount == 0);
	CHECK(lastBurstPackets == 2);
}


static void
test_two_full_packets_are_one_burst_of_two()
{
	uint32_t burstCount, lastBurstPackets;
	xhci_isochronous_burst(1920, 960, 3, &burstCount, &lastBurstPackets);
	CHECK(burstCount == 0);
	CHECK(lastBurstPackets == 1);
}


static void
test_a_short_last_packet_still_counts_as_a_packet()
{
	uint32_t burstCount, lastBurstPackets;
	xhci_isochronous_burst(2000, 960, 3, &burstCount, &lastBurstPackets);
	CHECK(burstCount == 0);
	CHECK(lastBurstPackets == 2);
}


static void
test_more_packets_than_one_burst_take_a_second_burst()
{
	// Endpoint without extra transactions: every packet is a burst of its own.
	uint32_t burstCount, lastBurstPackets;
	xhci_isochronous_burst(1920, 960, 1, &burstCount, &lastBurstPackets);
	CHECK(burstCount == 1);
	CHECK(lastBurstPackets == 0);
}


static void
test_an_empty_packet_is_a_burst_with_one_packet()
{
	uint32_t burstCount, lastBurstPackets;
	xhci_isochronous_burst(0, 960, 3, &burstCount, &lastBurstPackets);
	CHECK(burstCount == 0);
	CHECK(lastBurstPackets == 0);
}


static void
test_a_packet_size_of_zero_does_not_divide_by_zero()
{
	uint32_t burstCount = 99, lastBurstPackets = 99;
	xhci_isochronous_burst(2880, 0, 3, &burstCount, &lastBurstPackets);
	CHECK(burstCount == 0);
	CHECK(lastBurstPackets == 0);
}


static void
test_packet_size_leaves_out_the_extra_transactions()
{
	// wMaxPacketSize 0x13c0: 960 bytes, two extra transactions.
	CHECK(xhci_isochronous_packet_size(0x13c0) == 960);
	CHECK(xhci_isochronous_packet_size(640) == 640);
	CHECK(xhci_isochronous_packet_size(0x0a80) == 640);
}


#define RUN(test) \
	do { \
		int before = sFailures; \
		test(); \
		sTests++; \
		printf("%s %s\n", sFailures == before ? "ok  " : "FAIL", #test); \
	} while (0)


int
main()
{
	RUN(test_one_packet_is_one_burst_with_one_packet);
	RUN(test_three_full_packets_are_one_burst_of_three);
	RUN(test_two_full_packets_are_one_burst_of_two);
	RUN(test_a_short_last_packet_still_counts_as_a_packet);
	RUN(test_more_packets_than_one_burst_take_a_second_burst);
	RUN(test_an_empty_packet_is_a_burst_with_one_packet);
	RUN(test_a_packet_size_of_zero_does_not_divide_by_zero);
	RUN(test_packet_size_leaves_out_the_extra_transactions);

	printf("%d tests, %d failures\n", sTests, sFailures);
	return sFailures == 0 ? 0 : 1;
}
