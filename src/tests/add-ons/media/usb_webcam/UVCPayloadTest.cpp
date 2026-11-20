/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "UVCPayload.h"

#include <stdio.h>
#include <string.h>


static int sFailures = 0;
static int sTests = 0;


#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			sFailures++; \
		} \
	} while (0)


//	Headers as the test camera 04ca:7066 sends them: 12 bytes, flags 0x8c
//	(frame ID 0) or 0x8d (frame ID 1), plus bit 1 for the end of a frame.


static void
test_header_gives_flags_and_payload()
{
	uint8_t packet[16] = { 12, 0x8f };
	uvc_payload_header header;
	CHECK(uvc_parse_payload_header(packet, sizeof(packet), &header));
	CHECK(header.length == 12);
	CHECK(header.frameID == 1);
	CHECK(header.endOfFrame);
	CHECK(!header.error);
}


static void
test_header_only_packet_has_no_payload()
{
	uint8_t packet[12] = { 12, 0x8c };
	uvc_payload_header header;
	CHECK(uvc_parse_payload_header(packet, sizeof(packet), &header));
	CHECK(header.length == 12);
	CHECK(header.frameID == 0);
	CHECK(!header.endOfFrame);
}


static void
test_error_bit_is_read()
{
	uint8_t packet[12] = { 12, 0xcc };
	uvc_payload_header header;
	CHECK(uvc_parse_payload_header(packet, sizeof(packet), &header));
	CHECK(header.error);
}


static void
test_empty_and_one_byte_packets_are_rejected()
{
	uint8_t packet[2] = { 2, 0x8c };
	uvc_payload_header header;
	CHECK(!uvc_parse_payload_header(packet, 0, &header));
	CHECK(!uvc_parse_payload_header(packet, 1, &header));
}


static void
test_header_longer_than_the_packet_is_rejected()
{
	uint8_t packet[8] = { 12, 0x8c };
	uvc_payload_header header;
	CHECK(!uvc_parse_payload_header(packet, sizeof(packet), &header));
}


static void
test_header_shorter_than_two_bytes_is_rejected()
{
	uint8_t packet[8] = { 1, 0x8c };
	uvc_payload_header header;
	CHECK(!uvc_parse_payload_header(packet, sizeof(packet), &header));
}


static void
test_frame_with_error_is_not_shown()
{
	CHECK(uvc_frame_usable(614400, false));
	CHECK(!uvc_frame_usable(614400, true));
	CHECK(!uvc_frame_usable(0, false));
}


//	A 12-byte header as this camera sends it: length, flags (PTS and SCR
//	present), 4 bytes PTS, 4 bytes source clock, 2 bytes SOF counter.
static void
make_header_with_sof(uint8_t* packet, uint16_t sof)
{
	memset(packet, 0, 12);
	packet[0] = 12;
	packet[1] = 0x8c;
	packet[10] = sof & 0xff;
	packet[11] = sof >> 8;
}


static void
test_scr_gives_the_sof_counter()
{
	uint8_t packet[16];
	make_header_with_sof(packet, 0xf234);
	uvc_payload_header header;
	CHECK(uvc_parse_payload_header(packet, sizeof(packet), &header));
	CHECK(header.hasSOF);
	// Only 11 bits count, the rest is reserved.
	CHECK(header.sof == 0x234);
}


static void
test_scr_without_pts_follows_the_flags()
{
	uint8_t packet[8] = { 8, 0x88, 0, 0, 0, 0, 0x05, 0x01 };
	uvc_payload_header header;
	CHECK(uvc_parse_payload_header(packet, sizeof(packet), &header));
	CHECK(header.hasSOF);
	CHECK(header.sof == 0x105);
}


static void
test_header_without_scr_has_no_sof()
{
	uint8_t plain[2] = { 2, 0x80 };
	uint8_t ptsOnly[6] = { 6, 0x84 };
	uvc_payload_header header;
	CHECK(uvc_parse_payload_header(plain, sizeof(plain), &header));
	CHECK(!header.hasSOF);
	CHECK(uvc_parse_payload_header(ptsOnly, sizeof(ptsOnly), &header));
	CHECK(!header.hasSOF);
}


static void
test_header_too_short_for_its_scr_has_no_sof()
{
	// The camera claims PTS and SCR, but the header has room for PTS only.
	uint8_t packet[8] = { 8, 0x8c };
	uvc_payload_header header;
	CHECK(uvc_parse_payload_header(packet, sizeof(packet), &header));
	CHECK(!header.hasSOF);
}


static void
test_first_packet_and_forward_steps_are_in_order()
{
	uvc_sof_order order;
	uvc_sof_order_reset(&order);
	CHECK(uvc_sof_in_order(&order, 500));
	CHECK(uvc_sof_in_order(&order, 500));
	CHECK(uvc_sof_in_order(&order, 503));
}


static void
test_step_back_is_out_of_order()
{
	uvc_sof_order order;
	uvc_sof_order_reset(&order);
	uvc_sof_in_order(&order, 172);
	CHECK(!uvc_sof_in_order(&order, 169));
}


static void
test_counter_running_over_is_in_order()
{
	uvc_sof_order order;
	uvc_sof_order_reset(&order);
	uvc_sof_in_order(&order, 2046);
	CHECK(uvc_sof_in_order(&order, 2047));
	CHECK(uvc_sof_in_order(&order, 1));
}


static void
test_only_the_step_back_itself_is_reported()
{
	// Two swapped transfers: 10..12 arrives before 7..9. One report is
	// enough to drop the frame, and the stream must not stay "behind".
	uvc_sof_order order;
	uvc_sof_order_reset(&order);
	uvc_sof_in_order(&order, 12);
	CHECK(!uvc_sof_in_order(&order, 7));
	CHECK(uvc_sof_in_order(&order, 9));
	CHECK(uvc_sof_in_order(&order, 13));
}


static void
test_reset_forgets_the_last_counter()
{
	uvc_sof_order order;
	uvc_sof_order_reset(&order);
	uvc_sof_in_order(&order, 1500);
	uvc_sof_order_reset(&order);
	CHECK(uvc_sof_in_order(&order, 10));
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
	RUN(test_header_gives_flags_and_payload);
	RUN(test_header_only_packet_has_no_payload);
	RUN(test_error_bit_is_read);
	RUN(test_empty_and_one_byte_packets_are_rejected);
	RUN(test_header_longer_than_the_packet_is_rejected);
	RUN(test_header_shorter_than_two_bytes_is_rejected);
	RUN(test_frame_with_error_is_not_shown);
	RUN(test_scr_gives_the_sof_counter);
	RUN(test_scr_without_pts_follows_the_flags);
	RUN(test_header_without_scr_has_no_sof);
	RUN(test_header_too_short_for_its_scr_has_no_sof);
	RUN(test_first_packet_and_forward_steps_are_in_order);
	RUN(test_step_back_is_out_of_order);
	RUN(test_counter_running_over_is_in_order);
	RUN(test_only_the_step_back_itself_is_reported);
	RUN(test_reset_forgets_the_last_counter);

	printf("%d tests, %d failures\n", sTests, sFailures);
	return sFailures == 0 ? 0 : 1;
}
